// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#pragma once

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "Core/HW/DSPHLE/UCodes/UCodes.h"

class ZeldaAudioRenderer
{
public:
	void PrepareFrame();
	void AddVoice(u16 voice_id);
	void FinalizeFrame();

	void SetSineTable(std::array<s16, 0x80>&& sine_table) { m_sine_table = sine_table; }
	void SetResamplingCoeffs(std::array<s16, 0x100>&& coeffs) { m_resampling_coeffs = coeffs; }
	void SetAfcCoeffs(std::array<s16, 0x20>&& coeffs) { m_afc_coeffs = coeffs; }
	void SetVPBBaseAddress(u32 addr) { m_vpb_base_addr = addr; }
	void SetOutputVolume(u16 volume) { m_output_volume = volume; }
	void SetOutputLeftBufferAddr(u32 addr) { m_output_lbuf_addr = addr; }
	void SetOutputRightBufferAddr(u32 addr) { m_output_rbuf_addr = addr; }

	void DoState(PointerWrap& p);

private:
	struct VPB;

	// Utility functions for audio operations.

	// Apply volume to a buffer. The volume is a fixed point integer, usually
	// 1.15 or 4.12 in the DAC UCode.
	template <size_t N, size_t B>
	void ApplyVolumeInPlace(std::array<s16, N>* buf, u16 vol)
	{
		for (size_t i = 0; i < N; ++i)
		{
			s32 tmp = (u32)(*buf)[i] * (u32)vol;
			tmp >>= 16 - B;
			MathUtil::Clamp(&tmp, -0x8000, 0x7fff);
			(*buf)[i] = (s16)tmp;
		}
	}
	template <size_t N>
	void ApplyVolumeInPlace_1_15(std::array<s16, N>* buf, u16 vol)
	{
		ApplyVolumeInPlace<N, 1>(buf, vol);
	}
	template <size_t N>
	void ApplyVolumeInPlace_4_12(std::array<s16, N>* buf, u16 vol)
	{
		ApplyVolumeInPlace<N, 4>(buf, vol);
	}

	// Mixes two buffers together while applying a volume to one of them. The
	// volume ramps up/down in N steps using the provided step delta value.
	//
	// Note: On a real GC, the stepping happens in 32 steps instead. But hey,
	// we can do better here with very low risk. Why not? :)
	template <size_t N>
	s32 AddBuffersWithVolumeRamp(std::array<s16, N>* dst,
	                             const std::array<s16, N>& src,
	                             s32 vol, s32 step)
	{
		if (!vol && !step)
			return vol;

		for (size_t i = 0; i < N; ++i)
		{
			(*dst)[i] += ((vol >> 16) * src[i]) >> 16;
			vol += step;
		}

		return vol;
	}

	// Whether the frame needs to be prepared or not.
	bool m_prepared = false;

	// MRAM addresses where output samples should be copied.
	u32 m_output_lbuf_addr = 0;
	u32 m_output_rbuf_addr = 0;

	// Output volume applied to buffers before being uploaded to RAM.
	u16 m_output_volume = 0;

	// Mixing buffers.
	typedef std::array<s16, 0x50> MixingBuffer;
	MixingBuffer m_buf_front_left{};
	MixingBuffer m_buf_front_right{};
	MixingBuffer m_buf_back_left{};
	MixingBuffer m_buf_back_right{};
	MixingBuffer m_buf_front_left_reverb{};
	MixingBuffer m_buf_front_right_reverb{};
	MixingBuffer m_buf_back_left_reverb{};
	MixingBuffer m_buf_back_right_reverb{};

	// Base address where VPBs are stored linearly in RAM.
	u32 m_vpb_base_addr;
	void FetchVPB(u16 voice_id, VPB* vpb);
	void StoreVPB(u16 voice_id, const VPB& vpb);

	// Sine table transferred from MRAM. Contains sin(x) values for x in
	// [0.0;pi/4] (sin(x) in [1.0;0.0]), in 1.15 fixed format.
	std::array<s16, 0x80> m_sine_table{};

	// Fills up a buffer with the input samples for a voice, represented by its
	// VPB.
	void LoadInputSamples(MixingBuffer* buffer, VPB* vpb);

	// Raw samples (pre-resampling) that need to be generated to result in 0x50
	// post-resampling input samples.
	u16 NeededRawSamplesCount(const VPB& vpb);

	// Resamples raw samples to 0x50 input samples, using the resampling ratio
	// and current position information from the VPB.
	void Resample(VPB* vpb, const s16* src, MixingBuffer* dst);

	// Coefficients used for resampling. 
	std::array<s16, 0x100> m_resampling_coeffs{};

	// Downloads PCM8 encoded samples from ARAM. Handles looping and other
	// parameters appropriately.
	void DownloadPCM8SamplesFromARAM(s16* dst, VPB* vpb, u16 requested_samples_count);

	// Downloads AFC encoded samples from ARAM and decode them. Handles looping
	// and other parameters appropriately.
	void DownloadAFCSamplesFromARAM(s16* dst, VPB* vpb, u16 requested_samples_count);
	void DecodeAFC(VPB* vpb, s16* dst, size_t block_count);
	std::array<s16, 0x20> m_afc_coeffs{};

	// Downloads samples from MRAM while handling appropriate length / looping
	// behavior.
	void DownloadRawSamplesFromMRAM(s16* dst, VPB* vpb, u16 requested_samples_count);
};

class ZeldaUCode : public UCodeInterface
{
public:
	ZeldaUCode(DSPHLE *dsphle, u32 crc);
	virtual ~ZeldaUCode();
	u32 GetUpdateMs() override;

	void HandleMail(u32 mail) override;
	void Update() override;

	void DoState(PointerWrap &p) override;

private:
	// UCode state machine. The control flow in the Zelda UCode family is quite
	// complex, using interrupt handlers heavily to handle incoming messages
	// which, depending on the type, get handled immediately or are queued in a
	// command buffer. In this implementation, the synchronous+interrupts flow
	// of the original DSP implementation is rewritten in an asynchronous/coro
	// + state machine style. It is less readable, but the best we can do given
	// our constraints.
	enum class MailState : u32
	{
		WAITING,
		RENDERING,
		WRITING_CMD,
		HALTED,
	};
	MailState m_mail_current_state = MailState::WAITING;
	u32 m_mail_expected_cmd_mails = 0;

	// Utility function to set the current state. Useful for debugging and
	// logging as a hook point.
	void SetMailState(MailState new_state)
	{
		// WARN_LOG(DSPHLE, "MailState %d -> %d", m_mail_current_state, new_state);
		m_mail_current_state = new_state;
	}

	// Voice synchronization / audio rendering flow control. When rendering an
	// audio frame, only voices up to max_voice_id will be rendered until a
	// sync mail arrives, increasing the value of max_voice_id. Additionally,
	// these sync mails contain 16 bit values that are used as bitfields to
	// control voice skipping on a voice per voice level.
	u32 m_sync_max_voice_id = 0;
	std::array<u32, 256> m_sync_voice_skip_flags{};

	// Command buffer (circular queue with r/w indices). Filled by HandleMail
	// when the state machine is in WRITING_CMD state. Commands get executed
	// when entering WAITING state and we are not rendering audio.
	std::array<u32, 64> m_cmd_buffer{};
	u32 m_read_offset = 0;
	u32 m_write_offset = 0;
	u32 m_pending_commands_count = 0;
	bool m_cmd_can_execute = true;

	// Reads a 32 bit value from the command buffer. Advances the read pointer.
	u32 Read32()
	{
		if (m_read_offset == m_write_offset)
		{
			ERROR_LOG(DSPHLE, "Reading too many command params");
			return 0;
		}

		u32 res = m_cmd_buffer[m_read_offset];
		m_read_offset = (m_read_offset + 1) % (sizeof (m_cmd_buffer) / sizeof (u32));
		return res;
	}

	// Writes a 32 bit value to the command buffer. Advances the write pointer.
	void Write32(u32 val)
	{
		m_cmd_buffer[m_write_offset] = val;
		m_write_offset = (m_write_offset + 1) % (sizeof (m_cmd_buffer) / sizeof (u32));
	}

	// Tries to run as many commands as possible until either the command
	// buffer is empty (pending_commands == 0) or we reached a long lived
	// command that needs to hijack the mail control flow.
	//
	// Might change the current state to indicate crashy commands.
	void RunPendingCommands();

	// Sends the two mails from DSP to CPU to ack the command execution.
	enum class CommandAck : u32
	{
		STANDARD,
		DONE_RENDERING,
	};
	void SendCommandAck(CommandAck ack_type, u16 sync_value);

	// Audio rendering flow control state.
	u32 m_rendering_requested_frames = 0;
	u16 m_rendering_voices_per_frame = 0;
	u32 m_rendering_curr_frame = 0;
	u32 m_rendering_curr_voice = 0;

	bool RenderingInProgress() const { return m_rendering_curr_frame != m_rendering_requested_frames; }
	void RenderAudio();

	// Main object handling audio rendering logic and state.
	ZeldaAudioRenderer m_renderer;
};
