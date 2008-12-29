// Copyright 2007-2008 Ben Hutchings.
// Copyright 2008 Petter Reinholdtsen.
// See the file "COPYING" for licence details.

#include <cstddef>
#include <cstring>
#include <iostream>
#include <ostream>
#include <stdexcept>

#include <boost/bind.hpp>
#include <boost/thread/thread.hpp>

#include "auto_codec.hpp"
#include "frame.h"
#include "frame_timer.h"
#include "mixer.hpp"
#include "os_error.hpp"
#include "ring_buffer.hpp"
#include "video_effect.h"

mixer::mixer()
    : clock_state_(run_state_wait),
      clock_thread_(boost::bind(&mixer::run_clock, this)),
      mixer_state_(run_state_wait),
      mixer_thread_(boost::bind(&mixer::run_mixer, this)),
      monitor_(0)
{
    format_.system = NULL;
    format_.frame_aspect = dv_frame_aspect_auto;
    format_.sample_rate = dv_sample_rate_auto;
    sources_.reserve(5);
    sinks_.reserve(5);
}

mixer::~mixer()
{
    {
	boost::mutex::scoped_lock lock(source_mutex_);
	clock_state_ = run_state_stop;
	clock_state_cond_.notify_one(); // in case it's still waiting
    }
    {
	boost::mutex::scoped_lock lock(mixer_mutex_);
	mixer_state_ = run_state_stop;
	mixer_state_cond_.notify_one();
    }

    clock_thread_.join();
    mixer_thread_.join();
}

mixer::source_id mixer::add_source()
{
    boost::mutex::scoped_lock lock(source_mutex_);
    source_id id;
    for (id = 0; id != sources_.size(); ++id)
    {
	if (!sources_[id].is_live)
	{
	    sources_[id].is_live = true;
	    return id;
	}
    }
    sources_.resize(id + 1);
    return id;
}

void mixer::remove_source(source_id id)
{
    boost::mutex::scoped_lock lock(source_mutex_);
    sources_.at(id).is_live = false;
}

void mixer::put_frame(source_id id, const dv_frame_ptr & frame)
{
    bool was_full;
    bool should_notify_clock = false;

    {
	boost::mutex::scoped_lock lock(source_mutex_);

	source_data & source = sources_.at(id);
	was_full = source.frames.full();

	if (!was_full)
	{
	    frame->timestamp = frame_timer_get();
	    source.frames.push(frame);

	    // Start clock ticking once one source has reached the
	    // target queue length
	    if (clock_state_ == run_state_wait
		&& source.frames.size() == target_queue_len)
	    {
		settings_.video_source_id = id;
		settings_.audio_source_id = id;
		settings_.do_record = false;
		settings_.cut_before = false;
		clock_state_ = run_state_run;
		should_notify_clock = true; // after we unlock the mutex
	    }

	    // Auto-select format
	    if (clock_state_ == run_state_run)
	    {
		format_settings format;
		format.system = dv_frame_system(frame.get());
		format.frame_aspect = dv_frame_get_aspect(frame.get());
		format.sample_rate = dv_frame_get_sample_rate(frame.get());

		if (format_.system == NULL)
		    format_.system = format.system;
		else if (format_.system != format.system)
		    std::cerr << "WARN: Source " << 1 + id
			      << " using wrong video system\n";

		if (format_.frame_aspect == dv_frame_aspect_auto)
		    format_.frame_aspect = format.frame_aspect;
		else if (format_.frame_aspect != format.frame_aspect)
		    std::cerr << "WARN: Source " << 1 + id
			      << " using wrong frame aspect\n";

		if (format_.sample_rate == dv_sample_rate_auto)
		    format_.sample_rate = format.sample_rate;
		else if (format_.sample_rate != format.sample_rate)
		    std::cerr << "WARN: Source " << 1 + id
			      << "using wrong sample rate\n";
	    }
	}
    }

    if (should_notify_clock)
	clock_state_cond_.notify_one();

    if (was_full)
	std::cerr << "WARN: Dropped frame from source " << 1 + id
		  << " due to full queue\n";
}

mixer::sink_id mixer::add_sink(sink * sink)
{
    boost::mutex::scoped_lock lock(sink_mutex_);
    // XXX We may want to be able to reuse sink slots.
    sinks_.push_back(sink);
    return sinks_.size() - 1;
}

void mixer::remove_sink(sink_id id)
{
    boost::mutex::scoped_lock lock(sink_mutex_);
    sinks_.at(id) = 0;
}

// Video effect settings.  In future this is likely to be an abstract
// base class but for now the only effect we will have is picture-in-
// picture.
struct mixer::video_effect_settings
{
    source_id sec_source_id;
    rectangle dest_region;
};

std::tr1::shared_ptr<mixer::video_effect_settings>
mixer::create_video_effect_pic_in_pic(source_id sec_source_id,
				      rectangle dest_region)
{
    // XXX Need to validate parameters before they break anything
    std::tr1::shared_ptr<video_effect_settings> result(
	new video_effect_settings);
    result->sec_source_id = sec_source_id;
    result->dest_region = dest_region;
    return result;
}

mixer::format_settings mixer::get_format() const
{
    boost::mutex::scoped_lock lock(source_mutex_);
    mixer::format_settings result = format_;
    lock.unlock();
    return result;
}

void mixer::set_format(format_settings format)
{
    boost::mutex::scoped_lock lock(source_mutex_);
    format_ = format;
}

void mixer::set_video_source(source_id id)
{
    boost::mutex::scoped_lock lock(source_mutex_);
    if (id < sources_.size())
	settings_.video_source_id = id;
    else
	throw std::range_error("video source id out of range");
}

void mixer::set_video_effect(
    std::tr1::shared_ptr<video_effect_settings> effect)
{
    boost::mutex::scoped_lock lock(source_mutex_);
    settings_.video_effect = effect;
}

void mixer::set_audio_source(source_id id)
{
    boost::mutex::scoped_lock lock(source_mutex_);
    if (id < sources_.size())
	settings_.audio_source_id = id;
    else
	throw std::range_error("audio source id out of range");
}

void mixer::set_monitor(monitor * monitor)
{
    assert(monitor && !monitor_);
    monitor_ = monitor;
}

void mixer::enable_record(bool flag)
{
    boost::mutex::scoped_lock lock(source_mutex_);
    settings_.do_record = flag;
}

void mixer::cut()
{
    boost::mutex::scoped_lock lock(source_mutex_);
    settings_.cut_before = true;
}

namespace
{
    // Ensure the frame timer is initialised at startup
    struct timer_initialiser { timer_initialiser(); } timer_dummy;
    timer_initialiser::timer_initialiser()
    {
	frame_timer_init();
    }

    void dub_audio(dv_frame & dest_frame, const dv_frame & source_frame)
    {
	// Copy AAUX blocks.  These are every 16th block in each DIF
	// sequence, starting from block 6.

	const dv_system * system = dv_frame_system(&dest_frame);
	assert(dv_frame_system(&source_frame) == system);

	for (unsigned seq_num = 0; seq_num != system->seq_count; ++seq_num)
	{
	    for (unsigned block_num = 0; block_num != 9; ++block_num)
	    {
		std::ptrdiff_t block_pos =
		    DIF_SEQUENCE_SIZE * seq_num
		    + DIF_BLOCK_SIZE * (6 + block_num * 16);
		std::memcpy(dest_frame.buffer + block_pos,
			    source_frame.buffer + block_pos,
			    DIF_BLOCK_SIZE);
	    }
	}
    }

    void silence_audio(dv_frame & dest_frame)
    {
	const dv_system * system = dv_frame_system(&dest_frame);
	static const unsigned sample_rate = 48000;
	static const dv_sample_rate sample_rate_code = dv_sample_rate_48k;
	unsigned sample_count = (sample_rate * system->frame_rate_denom
				 / system->frame_rate_numer);

	// Each audio block has a 3-byte block id, a 5-byte AAUX
	// pack, and 72 bytes of samples.  Audio block 3 in each
	// sequence has an AS (audio source) pack, audio block 4
	// has an ASC (audio source control) pack, and the other
	// packs seem to be optional.
	static const uint8_t aaux_blank_pack[DIF_PACK_SIZE] = {
	    0xFF, 0xFF, 0xFF, 0xFF, 0xFF
	};
	uint8_t aaux_as_pack[DIF_PACK_SIZE] = {
	    // pack id; 0x50 for AAUX source
	    0x50,
	    // bits 0-5: number of samples in frame minus minimum value
	    // bit 6: flag "should be 1"
	    // bit 7: flag for unlocked audio sampling
	    (sample_count - system->sample_limits[sample_rate_code].min_count)
	    | (1 << 6) | (1 << 7),
	    // bits 0-3: audio mode
	    // bit 4: flag for independent channels
	    // bit 5: flag for "lumped" stereo (?)
	    // bits 6-7: number of audio channels per block minus 1
	    0,
	    // bits 0-4: system type; 0x0 for DV
	    // bit 5: frame rate; 0 for 29.97 fps, 1 for 25 fps
	    // bit 6: flag for multi-language audio
	    // bit 7: ?
	    dv_frame_system_code(&dest_frame) << 5,
	    // bits 0-2: quantisation; 0 for 16-bit LPCM
	    // bits 3-5: sample rate code
	    // bit 6: time constant of emphasis; must be 1
	    // bit 7: flag for no emphasis
	    (sample_rate_code << 3) | (1 << 6) | (1 << 7)
	};
	static const uint8_t aaux_asc_pack[DIF_PACK_SIZE] = {
	    // pack id; 0x51 for AAUX source control
	    0x51,
	    // bits 0-1: emphasis flag and ?
	    // bits 2-3: compression level; 0 for once
	    // bits 4-5: input type; 1 for digital
	    // bits 6-7: copy generation management system; 0 for unrestricted
	    (1 << 4),
	    // bits 0-2: ?
	    // bits 3-5: recording mode; 1 for original (XXX should indicate dub)
	    // bit 6: recording end flag, inverted
	    // bit 7: recording start flag, inverted
	    (1 << 3) | (1 << 6) | (1 << 7),
	    // bits 0-6: speed; 0x20 seems to be normal
	    // bit 7: direction: 1 for forward
	    0x20 | (1 << 7),
	    // bits 0-6: genre; 0x7F seems to be unknown
	    // bit 7: reserved
	    0x7F
	};

	for (unsigned seq_num = 0; seq_num != system->seq_count; ++seq_num)
	{
	    for (unsigned block_num = 0; block_num != 9; ++block_num)
	    {
		std::ptrdiff_t block_pos =
		    DIF_SEQUENCE_SIZE * seq_num
		    + DIF_BLOCK_SIZE * (6 + block_num * 16);
		std::memcpy(dest_frame.buffer + block_pos
			    + DIF_BLOCK_ID_SIZE,
			    block_num == 3 ? aaux_as_pack
			    : block_num == 4 ? aaux_asc_pack
			    : aaux_blank_pack,
			    DIF_PACK_SIZE);
		std::memset(dest_frame.buffer + block_pos
			    + DIF_BLOCK_ID_SIZE + DIF_PACK_SIZE,
			    0,
			    DIF_BLOCK_SIZE - DIF_BLOCK_ID_SIZE
			    - DIF_PACK_SIZE);
	    }
	}
    }
}

void mixer::run_clock()
{
    const struct dv_system * audio_source_system = 0;

    {
	boost::mutex::scoped_lock lock(source_mutex_);
	while (clock_state_ == run_state_wait)
	    clock_state_cond_.wait(lock);
    }

    // Interval to the next frame (in ns)
    unsigned int frame_interval = 0;
    // Weighted rolling average frame interval
    unsigned int average_frame_interval = 0;

    for (uint64_t tick_timestamp = frame_timer_get();
	 ;
	 tick_timestamp += frame_interval, frame_timer_wait(tick_timestamp))
    {
	mix_data m;

	// Select the mixer settings and source frame(s)
	{
	    boost::mutex::scoped_lock lock(source_mutex_);

	    if (clock_state_ == run_state_stop)
		break;

	    m.settings = settings_;
	    settings_.cut_before = false;

	    m.source_frames.resize(sources_.size());
	    for (source_id id = 0; id != sources_.size(); ++id)
	    {
		if (sources_[id].frames.empty())
		{
		    m.source_frames[id].reset();
		}
		else
		{
		    m.source_frames[id] = sources_[id].frames.front();
		    sources_[id].frames.pop();
		}
	    }
	}

	assert(m.settings.audio_source_id < m.source_frames.size()
	       && m.settings.video_source_id < m.source_frames.size());

	// Frame timer is based on the audio source.  Synchronisation
	// with the audio source matters more because audio
	// discontinuities are even more annoying than dropped or
	// repeated video frames.
	if (dv_frame * audio_source_frame =
	    m.source_frames[m.settings.audio_source_id].get())
	{
	    if (audio_source_system != dv_frame_system(audio_source_frame))
	    {
		audio_source_system = dv_frame_system(audio_source_frame);

		// Use standard frame timing initially.
		frame_interval = (1000000000
				  / audio_source_system->frame_rate_numer
				  * audio_source_system->frame_rate_denom);
		average_frame_interval = frame_interval;
	    }
	    else
	    {
		// The delay for this frame has a large effect on the
		// interval to the next frame because we want to
		// correct clock deviations quickly, but a much
		// smaller effect on the rolling average so that we
		// don't over-correct.  This has experimentally been
		// found to work well.
		static const unsigned next_average_weight = 3;
		static const unsigned next_delay_weight = 1;
		static const unsigned average_rolling_weight = 15;
		static const unsigned average_next_weight = 1;

		// Try to keep target_queue_len - 0.5 frame intervals
		// between delivery of source frames and mixing them.
		// The "obvious" way to feed the delay into the
		// frame_time is to divide it by target_queue_len-0.5.
		// But this is inverse to the effect we want it to
		// have: if the delay is long, we need to reduce,
		// not increase, frame_time.  So we calculate a kind
		// of inverse based on the amount of queue space
		// that should remain free.
		const uint64_t delay =
		    tick_timestamp > audio_source_frame->timestamp
		    ? tick_timestamp - audio_source_frame->timestamp
		    : 0;
		const unsigned free_queue_time =
		    full_queue_len * frame_interval > delay
		    ? full_queue_len * frame_interval - delay
		    : 0;
		frame_interval =
		    (average_frame_interval * next_average_weight
		     + (free_queue_time
			* 2 / (2 * (full_queue_len - target_queue_len) + 1)
			* next_delay_weight))
		    / (next_average_weight + next_delay_weight);

		average_frame_interval =
		    (average_frame_interval * average_rolling_weight
		     + frame_interval * average_next_weight)
		    / (average_rolling_weight + average_next_weight);
	    }
	}

	std::size_t free_len;

	{
	    boost::mutex::scoped_lock lock(mixer_mutex_);
	    free_len = mixer_queue_.capacity() - mixer_queue_.size();
	    if (free_len != 0)
	    {
		mixer_queue_.push(m); // really want to move m here
		mixer_state_ = run_state_run;
	    }
	}

	if (free_len != 0)
	{
	    mixer_state_cond_.notify_one();
	}
	else
	{
	    std::cerr << "ERROR: Dropped source frames due to"
		" full mixer queue\n";
	}
    }
}

namespace
{
    raw_frame_ref make_raw_frame_ref(const raw_frame_ptr & frame)
    {
	struct raw_frame_ref result;
	for (int i = 0; i != 4; ++i)
	{
	    result.planes.data[i] = frame->header.data[i];
	    result.planes.linesize[i] = frame->header.linesize[i];
	}
	result.pix_fmt = frame->pix_fmt;
	result.height = raw_frame_system(frame.get())->frame_height;
	return result;
    }

    raw_frame_ptr decode_video_frame(
 	const auto_codec & decoder, const dv_frame_ptr & dv_frame)
    {
	const struct dv_system * system = dv_frame_system(dv_frame.get());
	raw_frame_ptr result = allocate_raw_frame();
	int got_frame;
	decoder.get()->opaque = result.get();
	int used_size = avcodec_decode_video(decoder.get(),
					     &result->header, &got_frame,
					     dv_frame->buffer, system->size);
	assert(got_frame && size_t(used_size) == system->size);
	result->header.opaque =
	    const_cast<void *>(static_cast<const void *>(system));
	result->aspect = dv_frame_get_aspect(dv_frame.get());
	return result;
    }

    inline unsigned bcd(unsigned v)
    {
	assert(v < 100);
	return ((v / 10) << 4) + v % 10;
    }
  
    void set_times(dv_frame & dv_frame)
    {
	// XXX We should work this out in the clock loop.
	time_t now;
	time(&now);
	tm now_tm;
	localtime_r(&now, &now_tm);

	// Generate nominal frame count and frame rate.
	unsigned frame_num = dv_frame.serial_num;
	unsigned frame_rate;
	if (dv_frame.buffer[3] & 0x80)
	{
	    frame_rate = 25;
	}
	else
	{
	    // Skip the first 2 frame numbers of each minute, except in
	    // minutes divisible by 10.  This results in a "drop frame
	    // timecode" with a nominal frame rate of 30 Hz.
	    frame_num = frame_num + 2 * frame_num / (60 * 30 - 2)
		- 2 * (frame_num + 2) / (10 * 60 * 30 - 18);
	    frame_rate = 30;
	}
     
	// Timecode format is based on SMPTE LTC
	// <http://en.wikipedia.org/wiki/Linear_timecode>:
	// 0: pack id = 0x13
	// 1: LTC bits 0-3, 8-11
	//    bits 0-5: frame part (BCD)
	//    bit 6: drop frame timecode flag
	// 2: LTC bits 16-19, 24-27
	//    bits 0-6: second part (BCD)
	// 3: LTC bits 32-35, 40-43
	//    bits 0-6: minute part (BCD)
	// 4: LTC bits 48-51, 56-59
	//    bits 0-5: hour part (BCD)
	// the remaining bits are meaningless in DV and we use zeroes
	uint8_t timecode[DIF_PACK_SIZE] = {
	    0x13,
	    bcd(frame_num % frame_rate) | (1 << 6),
	    bcd(frame_num / frame_rate % 60),
	    bcd(frame_num / (60 * frame_rate) % 60),
	    bcd(frame_num / (60 * 60 * frame_rate) % 24)
	};

	// Record date format:
	// 0: pack id = 0x62 (video) or 0x52 (audio)
	// 1: some kind of time zone indicator or 0xff for unknown
	// 2: bits 6-7: unused? reserved?
	//    bits 0-5: day part (BCD)
	// 3: bits 5-7: unused? reserved? day of week?
	//    bits 0-4: month part (BCD)
	// 4: year part (BCD)
	uint8_t video_record_date[DIF_PACK_SIZE] = {
	    0x62,
	    0xff,
	    bcd(now_tm.tm_mday),
	    bcd(1 + now_tm.tm_mon),
	    bcd(now_tm.tm_year % 100)
	};
	uint8_t audio_record_date[DIF_PACK_SIZE] = {
	    0x52,
	    0xff,
	    bcd(now_tm.tm_mday),
	    bcd(1 + now_tm.tm_mon),
	    bcd(now_tm.tm_year % 100)
	};	

	// Record time format (similar to timecode format):
	// 0: pack id = 0x63 (video) or 0x53 (audio)
	// 1: bits 6-7: reserved, set to 1
	//    bits 0-5: frame part (BCD) or 0x3f for unknown
	// 2: bit 7: unused? reserved?
	//    bits 0-6: second part (BCD)
	// 3: bit 7: unused? reserved?
	//    bits 0-6: minute part (BCD)
	// 4: bits 6-7: unused? reserved?
	//    bits 0-5: hour part (BCD)
	uint8_t video_record_time[DIF_PACK_SIZE] = {
	    0x63,
	    0xff,
	    bcd(now_tm.tm_sec),
	    bcd(now_tm.tm_min),
	    bcd(now_tm.tm_hour)
	};
	uint8_t audio_record_time[DIF_PACK_SIZE] = {
	    0x53,
	    0xff,
	    bcd(now_tm.tm_sec),
	    bcd(now_tm.tm_min),
	    bcd(now_tm.tm_hour)
	};

        // In DIFs 1 and 2 (subcode) of sequence 6 onward:
	// - Write timecode at offset 6 and 30
	// - Write video record date at offset 14 and 38
	// - Write video record time at offset 22 and 46
	// In DIFs 3, 4 and 5 (VAUX) of even sequences:
	// - Write video record date at offset 13 and 58
	// - Write video record time at offset 18 and 63
	// In DIF 86 of even sequences and DIF 38 of odd sequences (AAUX):
	// - Write audio record date at offset 3
	// In DIF 102 of even sequences and DIF 54 of odd sequences (AAUX):
	// - Write audio record time at offset 3

	for (unsigned seq_num = 0;
	     seq_num != dv_frame_system(&dv_frame)->seq_count;
	     ++seq_num)
	{
	    if (seq_num >= 6)
	    {
		for (unsigned block_num = 1; block_num <= 3; ++block_num)
		{
		    for (unsigned i = 0; i <= 1; ++i)
		    {
			memcpy(dv_frame.buffer + seq_num * DIF_SEQUENCE_SIZE
			       + block_num * DIF_BLOCK_SIZE + i * 24 + 6,
			       timecode,
			       DIF_PACK_SIZE);
			memcpy(dv_frame.buffer + seq_num * DIF_SEQUENCE_SIZE
			       + block_num * DIF_BLOCK_SIZE + i * 24 + 14,
			       video_record_date,
			       DIF_PACK_SIZE);
			memcpy(dv_frame.buffer + seq_num * DIF_SEQUENCE_SIZE
			       + block_num * DIF_BLOCK_SIZE + i * 24 + 22,
			       video_record_time,
			       DIF_PACK_SIZE);
		    }
		}
	    }

	    for (unsigned block_num = 3; block_num <= 5; ++block_num)
	    {
		for (unsigned i = 0; i <= 1; ++i)
		{
		    memcpy(dv_frame.buffer + seq_num * DIF_SEQUENCE_SIZE
			   + block_num * DIF_BLOCK_SIZE + i * 45 + 13,
			   video_record_date,
			   DIF_PACK_SIZE);
		    memcpy(dv_frame.buffer + seq_num * DIF_SEQUENCE_SIZE
			   + block_num * DIF_BLOCK_SIZE + i * 45 + 18,
			   video_record_time,
			   DIF_PACK_SIZE);
		}
	    }

	    memcpy(dv_frame.buffer + seq_num * DIF_SEQUENCE_SIZE
		   + ((seq_num & 1) ? 38 : 86) * DIF_BLOCK_SIZE + 3,
		   audio_record_date,
		   DIF_PACK_SIZE);
	    memcpy(dv_frame.buffer + seq_num * DIF_SEQUENCE_SIZE
		   + ((seq_num & 1) ? 54 : 102) * DIF_BLOCK_SIZE + 3,
		   audio_record_time,
		   DIF_PACK_SIZE);
	}
    }
}

void mixer::run_mixer()
{
    dv_frame_ptr last_mixed_dv;
    unsigned serial_num = 0;
    const mix_data * m = 0;

    auto_codec decoder(auto_codec_open_decoder(CODEC_ID_DVVIDEO));
    AVCodecContext * dec = decoder.get();
    dec->get_buffer = raw_frame_get_buffer;
    dec->release_buffer = raw_frame_release_buffer;
    dec->reget_buffer = raw_frame_reget_buffer;
    auto_codec encoder(auto_codec_open_encoder(CODEC_ID_DVVIDEO));

    for (;;)
    {
	// Get the next set of source frames and mix settings (or stop
	// if requested)
	{
	    boost::mutex::scoped_lock lock(mixer_mutex_);

	    if (m)
		mixer_queue_.pop();

	    while (mixer_state_ != run_state_stop && mixer_queue_.empty())
		mixer_state_cond_.wait(lock);
	    if (mixer_state_ == run_state_stop)
		break;

	    m = &mixer_queue_.front();
	}

	for (unsigned id = 0; id != m->source_frames.size(); ++id)
	    if (m->source_frames[id])
		m->source_frames[id]->serial_num = serial_num;

	const dv_frame_ptr & audio_source_dv =
	    m->source_frames[m->settings.audio_source_id];
	const dv_frame_ptr & video_pri_source_dv =
	    m->source_frames[m->settings.video_source_id];

	dv_frame_ptr mixed_dv;
	raw_frame_ptr video_pri_source_raw;
	raw_frame_ptr video_sec_source_raw;
	raw_frame_ptr mixed_raw;

	if (!video_pri_source_dv)
	{
	    std::cerr << "WARN: Repeating frame due to empty queue"
		" for source " << 1 + m->settings.video_source_id << "\n";

	    // Make a copy of the last mixed frame so we can
	    // replace the audio.  (We can't modify the last frame
	    // because sinks may still be reading from it.)
	    mixed_dv = allocate_dv_frame();
	    std::memcpy(mixed_dv.get(),
			last_mixed_dv.get(),
			offsetof(dv_frame, buffer)
			+ dv_frame_system(last_mixed_dv.get())->size);
	    mixed_dv->serial_num = serial_num;
	}
	else if (m->settings.video_effect
		 && m->source_frames[m->settings.video_effect->sec_source_id])
	{
	    const dv_frame_ptr video_sec_source_dv =
		m->source_frames[m->settings.video_effect->sec_source_id];

	    // Decode sources
	    mixed_raw = decode_video_frame(decoder, video_pri_source_dv);
	    video_sec_source_raw =
		decode_video_frame(decoder, video_sec_source_dv);

	    // Mix raw video
	    video_effect_pic_in_pic(
		make_raw_frame_ref(mixed_raw),
		m->settings.video_effect->dest_region,
		make_raw_frame_ref(video_sec_source_raw),
		raw_frame_system(video_sec_source_raw.get())->active_region);

	    // Encode mixed video
	    const dv_system * system = raw_frame_system(mixed_raw.get());
	    AVCodecContext * enc = encoder.get();
	    if (dv_frame_get_aspect(video_pri_source_dv.get())
		== dv_frame_aspect_wide)
	    {
		enc->sample_aspect_ratio.num = system->pixel_aspect_wide.width;
		enc->sample_aspect_ratio.den = system->pixel_aspect_wide.height;
	    }
	    else
	    {
		enc->sample_aspect_ratio.num = system->pixel_aspect_normal.width;
		enc->sample_aspect_ratio.den = system->pixel_aspect_normal.height;
	    }
	    enc->time_base.num = system->frame_rate_denom;
	    enc->time_base.den = system->frame_rate_numer;
	    enc->width = system->frame_width;
	    enc->height = system->frame_height;
	    enc->pix_fmt = mixed_raw->pix_fmt;
	    mixed_raw->header.pts = serial_num;
  	    mixed_dv = allocate_dv_frame();
	    int out_size = avcodec_encode_video(enc,
						mixed_dv->buffer, system->size, 
						&mixed_raw->header);
	    assert(size_t(out_size) == system->size);
	    mixed_dv->serial_num = serial_num;
	}
	else
	{
	    mixed_dv = video_pri_source_dv;
	}

	if (mixed_dv != audio_source_dv)
	    if (audio_source_dv
		&& dv_frame_system(audio_source_dv.get())
		== dv_frame_system(mixed_dv.get()))
		dub_audio(*mixed_dv, *audio_source_dv);
	    else
		silence_audio(*mixed_dv);

	set_times(*mixed_dv);

	mixed_dv->do_record = m->settings.do_record;
	mixed_dv->cut_before = m->settings.cut_before;

	last_mixed_dv = mixed_dv;
	++serial_num;

	// Sink the frame
	{
	    boost::mutex::scoped_lock lock(sink_mutex_);
	    for (sink_id id = 0; id != sinks_.size(); ++id)
		if (sinks_[id])
		    sinks_[id]->put_frame(mixed_dv);
	}
	if (monitor_)
	    monitor_->put_frames(m->source_frames.size(), &m->source_frames[0],
				 m->settings, mixed_dv, mixed_raw);
    }
}
