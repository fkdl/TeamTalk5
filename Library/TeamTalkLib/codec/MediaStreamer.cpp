/*
 * Copyright (c) 2005-2018, BearWare.dk
 * 
 * Contact Information:
 *
 * Bjoern D. Rasmussen
 * Kirketoften 5
 * DK-8260 Viby J
 * Denmark
 * Email: contact@bearware.dk
 * Phone: +45 20 20 54 59
 * Web: http://www.bearware.dk
 *
 * This source code is part of the TeamTalk SDK owned by
 * BearWare.dk. Use of this file, or its compiled unit, requires a
 * TeamTalk SDK License Key issued by BearWare.dk.
 *
 * The TeamTalk SDK License Agreement along with its Terms and
 * Conditions are outlined in the file License.txt included with the
 * TeamTalk SDK distribution.
 *
 */

#include "MediaStreamer.h"

#if defined(ENABLE_DSHOW)
#include "WinMedia.h"
#endif
#if defined(ENABLE_FFMPEG1)
#include "FFMpeg1Streamer.h"
#endif
#if defined(ENABLE_FFMPEG3)
#include "FFMpeg3Streamer.h"
#endif

#include "MediaUtil.h"

#include <assert.h>

using namespace media;

bool GetMediaFileProp(const ACE_TString& filename, MediaFileProp& fileprop)
{
#if defined(ENABLE_DSHOW)
    return GetDSMediaFileProp(filename, fileprop);
#elif defined(ENABLE_FFMPEG1) || defined(ENABLE_FFMPEG3)
    return GetAVMediaFileProp(filename, fileprop);
#endif
    return false;
}

media_streamer_t MakeMediaStreamer(MediaStreamListener* listener)
{
    media_streamer_t streamer;
    MediaStreamer* tmp_streamer = NULL;

#ifdef ENABLE_DSHOW
    ACE_NEW_RETURN(tmp_streamer, DSWrapperThread(listener), media_streamer_t());
#elif defined(ENABLE_FFMPEG1) || defined(ENABLE_FFMPEG3)
    ACE_NEW_RETURN(tmp_streamer, FFMpegStreamer(listener), media_streamer_t());
#endif
    streamer = media_streamer_t(tmp_streamer);

    return streamer;
}

void MediaStreamer::Reset()
{
    m_media_in = MediaFileProp();
    m_media_out = MediaStreamOutput();
    m_stop = false;
}

bool MediaStreamer::ProcessAVQueues(ACE_UINT32 starttime, int wait_ms, 
                                    bool flush)
{
    ACE_UINT32 audio_time = ProcessAudioFrame(starttime, flush);
    ACE_UINT32 video_time = ProcessVideoFrame(starttime);

    //go to sleep if we didn't send anything
    if(wait_ms && !audio_time && !video_time)
    {
//         MYTRACE(ACE_TEXT("Sleeping %d msec... waiting for video frames\n"), 
//                 wait_ms);
        ACE_OS::sleep(ACE_Time_Value(wait_ms / 1000, 
                                     (wait_ms % 1000) * 1000));
        return false;
    }

    return audio_time || video_time;
}

ACE_UINT32 MediaStreamer::ProcessAudioFrame(ACE_UINT32 starttime, bool flush)
{
    //see if audio block is less than time 'now'
    ACE_Time_Value tv;
    ACE_Message_Block* mb;
    if(m_audio_frames.peek_dequeue_head(mb, &tv) < 0)
        return 0;

    AudioFrame* first_frame = reinterpret_cast<AudioFrame*>(mb->base());
    if (W32_GT(first_frame->timestamp, GETTIMESTAMP() - starttime))
        return 0;
    
    // calculate if we can build a frame. 'message_length()' is data available
    // in message queue which has been written but hasn't been read yet.
    // 'message_bytes()' is total amount of data allocated in the message queue.
    size_t hdrs_size = m_audio_frames.message_count() * sizeof(AudioFrame);
    //if we have already read some data of a block we need to substract its header
    if(mb->rd_ptr() != mb->base())
        hdrs_size -= sizeof(AudioFrame);

    int audio_bytes = int(m_audio_frames.message_length() - hdrs_size);

    int required_audio_bytes = PCM16_BYTES(m_media_out.audio_samples,
                                           m_media_out.audio_channels);
    
    if(audio_bytes < required_audio_bytes && !flush)
        return 0;

    int audio_block_size = required_audio_bytes + sizeof(AudioFrame);
    ACE_Message_Block* out_mb;
    ACE_NEW_RETURN(out_mb, ACE_Message_Block(audio_block_size), 0);

    AudioFrame* media_frame = reinterpret_cast<AudioFrame*>(out_mb->wr_ptr());
    *media_frame = *first_frame; //use original AudioFrame as base for construction

    //set output properties
    media_frame->timestamp = first_frame->timestamp + starttime;
    media_frame->input_buffer = 
        reinterpret_cast<short*>(out_mb->wr_ptr() + sizeof(AudioFrame));
    media_frame->input_samples = m_media_out.audio_samples;
    //advance wr_ptr() past header
    out_mb->wr_ptr(sizeof(*media_frame));
    //out_mb->copy(reinterpret_cast<char*>(&media_frame), sizeof(media_frame));

    size_t write_bytes = required_audio_bytes;
    do
    {
        //check if we should advance past header
        if(mb->rd_ptr() == mb->base())
            mb->rd_ptr(sizeof(AudioFrame));
            
        if(mb->length() <= write_bytes)
        {
            out_mb->copy(mb->rd_ptr(), mb->length());
            write_bytes -= mb->length();
            assert((int)write_bytes >= 0);
            mb->rd_ptr(mb->length());
            //assert(mb->rd_ptr() == mb->end());
            if(m_audio_frames.dequeue(mb, &tv) >= 0)
                mb->release();
        }
        else
        {
            out_mb->copy(mb->rd_ptr(), write_bytes);
            mb->rd_ptr(write_bytes);
            write_bytes -= write_bytes;
            assert(mb->rd_ptr() < mb->end());
        }
    }
    while(write_bytes > 0 && m_audio_frames.peek_dequeue_head(mb, &tv) >= 0);

    //write bytes should only be greater than 0 if flushing
    if(write_bytes)
    {
        ACE_OS::memset(out_mb->wr_ptr(), 0, write_bytes);
        out_mb->wr_ptr(write_bytes);
        assert(out_mb->end() == out_mb->wr_ptr());
    }

    ACE_UINT32 timestamp = media_frame->timestamp;
    //MYTRACE(ACE_TEXT("Ejecting audio frame %u\n"), media_frame.timestamp);
    if(!m_media_out.audio || 
       !m_listener->MediaStreamAudioCallback(this, *media_frame, out_mb))
    {
        out_mb->release();
    }
    //'out_mb' should now be considered dead
    return timestamp;
}

ACE_UINT32 MediaStreamer::ProcessVideoFrame(ACE_UINT32 starttime)
{
    ACE_UINT32 msg_time = 0;
    ACE_Message_Block* mb;
    ACE_Time_Value tm_zero;
    while(m_video_frames.peek_dequeue_head(mb, &tm_zero) >= 0)
    {
        VideoFrame* media_frame = reinterpret_cast<VideoFrame*>(mb->rd_ptr());
        
        if (W32_LEQ(media_frame->timestamp, GETTIMESTAMP() - starttime))
        {
            tm_zero = ACE_Time_Value::zero;
            if(m_video_frames.dequeue(mb, &tm_zero) < 0)
                return msg_time;

            msg_time = starttime + media_frame->timestamp;
            media_frame->timestamp = msg_time;

            //MYTRACE(ACE_TEXT("Ejecting video frame %u\n"), media_frame->timestamp);
            if(!m_media_out.video || 
               !m_listener->MediaStreamVideoCallback(this, *media_frame, mb))
            {
                mb->release();
            }
        }
        else
        {
            MYTRACE(ACE_TEXT("Not video time %u, duration %u\n"),
                    media_frame->timestamp, GETTIMESTAMP() - starttime);
            break;
        }
    }

    return msg_time;
}
