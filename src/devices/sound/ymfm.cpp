// license:BSD-3-Clause
// copyright-holders:Aaron Giles

#include "emu.h"
#include "ymfm.h"
#include "ymfm.ipp"

namespace ymfm
{

//
// ONE FM CORE TO RULE THEM ALL
//
// This emulator is written from the ground-up using the analysis and deduction
// by Nemesis as a starting point, particularly in this thread:
//
//    https://gendev.spritesmind.net/forum/viewtopic.php?f=24&t=386
//
// The core assumption is that these details apply to all FM variants unless
// otherwise proven incorrect.
//
// The fine details of this implementation have also been cross-checked against
// Nemesis' implementation in his Exodus emulator, as well as Alexey Khokholov's
// "Nuked" implementations based off die shots.
//
// Operator and channel summing/mixing code is largely based off of research
// done by David Viens and Hubert Lamontagne.
//
// Search for QUESTION to find areas where I am unsure.
//
//
// FAMILIES
//
// The Yamaha FM chips can be broadly categoried into families:
//
//   OPM (YM2151)
//   OPN (YM2203)
//     OPNA/OPNB/OPN2 (YM2608, YM2610, YM2610B, YM2612, YM3438, YMF276, YMF288)
//   OPL (YM3526)
//     OPL2 (YM3812)
//     OPLL (YM2413, YM2423, YMF281, DS1001, and others)
//     OPL3 (YMF262, YMF278)
//
// All of these families are very closely related, and the ymfm engine
// implemented below is designed to be universal to work across all of
// these families.
//
// Of course, each variant has its own register maps, features, and
// implementation details which need to be sorted out. Thus, each
// significant variant listed above is represented by a register class. The
// register class contains:
//
//   * constants describing core parameters and features
//   * mappers between operators and channels
//   * generic fetchers that return normalized values across families
//   * family-specific helper functions
//
//
// FAMILY HISTORY
//
// OPM started it all off, featuring:
//   - 8 FM channels, 4 operators each
//   - LFO and noise support
//   - Stereo output
//
// OPM -> OPN changes:
//   - Reduced to 3 FM channels, 4 operators each
//   - Removed LFO and noise support
//   - Mono output
//   - Integrated AY-8910 compatible PSG
//   - Added SSG-EG envelope mode
//   - Added multi-frequency mode: ch. 3 operators can have separate frequencies
//   - Software controlled clock divider
//
// OPN -> OPNA changes:
//   - Increased to 6 FM channels, 4 operators each
//   - Added back (a cut-down) LFO
//   - Stereo output again
//   - Removed software controlled divider on later versions (OPNB/OPN2)
//   - Removed PSG on OPN2 models
//
// OPNA -> OPL changes:
//   - Increased to 9 FM channels, but only 2 operators each
//   - Even more simplified LFO
//   - Mono output
//   - Removed PSG
//   - Removed SSG-EG envelope modes
//   - Removed multi-frequency modes
//   - Fixed clock divider
//   - Built-in ryhthm generation
//
// OPL -> OPL2 changes:
//   - Added 4 selectable waveforms
//
// OPL2 -> OPLL changes:
//   - Vastly simplified register map
//   - 15 built-in instruments, plus built-in rhythm instruments
//   - 1 user-controlled instrument
//
// OPL2 -> OPL3 changes:
//   - Increased to 18 FM channels, 2 operators each
//   - 4 output channels
//   - Increased to 8 selectable waveforms
//   - 6 channels can be configured to use 4 operators
//
//
// CHANNELS AND OPERATORS
//
// The polyphony of a given chip is determined by the number of channels
// it supports. This number ranges from as low as 3 to as high as 18.
// Each channel has either 2 or 4 operators that can be combined in a
// myriad of ways. On most chips the number of operators per channel is
// fixed; however, some later OPL chips allow this to be toggled between
// 2 and 4 at runtime.
//
// The base ymfm engine class maintains an array of channels and operators,
// while the relationship between the two is described by the register
// class.
//
//
// REGISTERS
//
// Registers on the Yamaha chips are generally write-only, and can be divided
// into three distinct categories:
//
//   * system-wide registers
//   * channel-specific registers
//   * operator-specific registers
//
// For maximum flexibility, most parameters can be configured at the operator
// level, with channel-level registers controlling details such as how to
// combine the operators into the final output. System-wide registers are
// used to control chip-wide modes and manage onboard timer functions.
//
// Note that since registers are write-only, some implementations will use
// "holes" in the register space to store additional values that may be
// needed.
//
//
// ATTENUATION
//
// Most of the computations of the FM engines are done in terms of attenuation,
// and thus are logarithmic in nature. The maximum resolution used intnerally
// is 12 bits, as returned by the sin table:
//
//     Bit  11  10   9   8   7   6    5     4      3       2        1         0
//      dB -96 -48 -24 -12  -6  -3 -1.5 -0.75 -0.375 -0.1875 -0.09375 -0.046875
//
// The envelope generator internally uses 10 bits:
//
//     Bit   9   8   7   6   5   4    3     2      1       0
//      dB -96 -48 -24 -12  -6  -3 -1.5 -0.75 -0.375 -0.1875
//
// Total level for operators is usually represented by 7 bits:
//
//         Bit   6   5   4   3   2    1     0
//          dB -48 -24 -12  -6  -3 -1.5 -0.75
//
// Sustain level in the envelope generator is usually represented by 4 bits:
//
//             Bit   3   2   1   0
//              dB -24 -12  -6  -3
//
//
// STATUS AND TIMERS
//
// Generically, all chips (except OPLL) support two timers that can be
// programmed to fire and signal IRQs. These timers also set bits in the
// status register. The behavior of these bits is shared across all
// implementations, even if the exact bit positions shift (this is controlled
// by constants in the registers class).
//
// In addition, several chips incorporate ADPCM decoders which also may set
// bits in the same status register. For this reason, it is possible to
// control various bits in the status register via the set_reset_status()
// function directly. Any active bits that are set and which are not masked
// (mask is controlled by set_irq_mask()), lead to an IRQ being signalled.
//
// Thus, it is possible for the chip-specific implementations to set the
// mask and control the status register bits such that IRQs are signalled
// via the same mechanism as timer signals.
//
// In addition, the OPM and OPN families have a "busy" flag, which is set
// after each write, indicating that another write should not be performed.
// Historically, the duration of this flag was constant and had nothing to
// do with the internals of the chip. However, since the details can
// potentially vary chip-to-chip, it is the chip's responsibility to
// insert the busy flag into the status before returning it to the caller.
//
//
// CLOCKING
//
// Each of the Yamaha chips works by cycling through all operators one at
// a time. Thus, the effective output rate of the chips is related to the
// input clock divided by the number of operators. In addition, the input
// clock is prescaled by an amount. Generally, this is a fixed value, though
// some early OPN chips allow this to be selected at runtime from a small
// number of values.
//
//
// CHANNEL FREQUENCIES
//
// One major difference between OPM and later families is in how frequencies
// are specified. OPM specifies frequency via a 3-bit 'block' (aka octave),
// combined with a 4-bit 'key code' (note number) and a 6-bit 'key fraction'.
// The key code and fraction are converted on the chip into an x.11 fixed-
// point value and then shifted by the block to produce the final step value
// for the phase.
//
// Later families, on the other hand, specify frequencies via a 3-bit 'block'
// just as on OPM, but combined with a 9, 10, or 11-bit 'frequency number'
// or 'fnum', which is directly shifted by the block to produce the step
// value. So essentially, later chips make the user do the conversion from
// note value to phase increment, while OPM is programmed in a more 'musical'
// way, specifying notes and cents.
//
// Interally, this is abstracted away into a 'block_freq' value, which is a
// 16-bit value containing the block and frequency info concatenated together
// as follows:
//
//    OPM:  [3-bit block]:[4-bit keycode]:[6-bit fraction] = 13 bits total
//
//    OPN:  [3-bit block]:[11-bit fnum]    = 14 bits total
//    OPL:  [3-bit block]:[10-bit fnum]:0  = 14 bits total
//    OPLL: [3-bit block]:[ 9-bit fnum]:00 = 14 bits total
//
// Template specialization in functions that interpret the 'block_freq' value
// is used to deconstruct it appropriately (specifically, see clock_phase).
//
//
// LOW FREQUENCY OSCILLATOR (LFO)
//
// The LFO engines are different in several key ways. The OPM LFO engine is
// fairly intricate. It has a 4.4 floating-point rate which allows for a huge
// range of frequencies, and can select between four different waveforms
// (sawtooth, square, triangle, or noise). Separate 7-bit depth controls for
// AM and PM control the amount of modulation applied in each case. This
// global LFO value is then further controlled at the channel level by a 2-bit
// AM sensitivity and a 3-bit PM sensitivity, and each operator has a 1-bit AM
// on/off switch.
//
// For OPN the LFO engine was removed entirely, but a limited version was put
// back in OPNA and later chips. This stripped-down version offered only a
// 3-bit rate setting (versus the 4.4 floating-point rate in OPN), and no
// depth control. It did bring back the channel-level sensitivity controls and
// the operator-level on/off control.
//
// For OPL, the LFO is simplified again, with AM and PM running at fixed
// frequencies, and simple enable flags at the operator level for each
// controlling their application.
//
//
// DIFFERENCES BETWEEN FAMILIES
//
// The table below provides some high level functional differences between the
// differnet families:
//
//              +--------++-----------------++-----------------------------------+
//      family: |   OPM  ||       OPN       ||                OPL                |
//              +--------++--------+--------++--------+--------+--------+--------+
//   subfamily: |   OPM  ||   OPN  |  OPNA  ||   OPL  |  OPL2  |  OPLL  |  OPL3  |
//              +--------++--------+--------++--------+--------+--------+--------+
//     outputs: |    2   ||    1   |    2   ||    1   |    1   |    1   |    4   |
//    channels: |    8   ||    3   |    6   ||    9   |    9   |    9   |   18   |
//   operators: |   32   ||   12   |   24   ||   18   |   18   |   18   |   36   |
//   waveforms: |    1   ||    1   |    1   ||    1   |    4   |    2   |    8   |
// instruments: |   no   ||   no   |   no   ||   yes  |   yes  |   yes  |   yes  |
//      ryhthm: |   no   ||   no   |   no   ||   no   |   no   |   yes  |   no   |
// dynamic ops: |   no   ||   no   |   no   ||   no   |   no   |   no   |   yes  |
//    prescale: |    2   ||  2/3/6 |  2/3/6 ||    4   |    4   |    4   |    8   |
//  EG divider: |    3   ||    3   |    3   ||    1   |    1   |    1   |    1   |
//       EG DP: |   no   ||   no   |   no   ||   no   |   no   |   yes  |   no   |
//      EG SSG: |   no   ||   yes  |   yes  ||   no   |   no   |   no   |   no   |
//   mod delay: |   no   ||   no   |   no   ||   yes  |   yes  |   yes? |   no   |
//         CSM: |   yes  ||  ch 2  |  ch 2  ||   yes  |   yes  |   yes  |   no   |
//         LFO: |   yes  ||   no   |   yes  ||   yes  |   yes  |   yes  |   yes  |
//       noise: |   yes  ||   no   |   no   ||   no   |   no   |   no   |   no   |
//              +--------++--------+--------++--------+--------+--------+--------+
//
// Outputs represents the number of output channels: 1=mono, 2=stereo, 4=stereo+.
// Channels represents the number of independent FM channels.
// Operators represents the number of operators, or "slots" which are assembled
//   into the channels.
// Waveforms represents the number of different sine-derived waveforms available.
// Instruments indicates whether the family has built-in instruments.
// Rhythm indicates whether the family has a built-in rhythm
// Dynamic ops indicates whether it is possible to switch between 2-operator and
//   4-operator modes dynamically.
// Prescale specifies the default clock divider; some chips allow this to be
//   controlled via register writes.
// EG divider represents the divider applied to the envelope generator clock.
// EG DP indicates whether the envelope generator includes a DP (depress?) phase
//   at the beginning of each key on.
// SSG EG indicates whether the envelope generator has SSG-style support.
// Mod delay indicates whether the connection to the first modulator's input is
//   delayed by 1 sample.
// CSM indicates whether CSM mode is supported, triggered by timer A.
// LFO indicates whether LFO is supported.
// Noise indicates whether one of the operators can be replaced with a noise source.
//
//
// CHIP SPECIFICS
//
// While OPM is its own thing, the OPN and OPL families have quite a few specific
// implementations, with many differing details beyond the core FM parts. Here are
// some details on the OPN family:
//
//           +--------++--------+--------++--------+---------++--------+--------+--------+
//  chip ID: | YM2203 || YM2608 | YMF288 || YM2610 | YM2610B || YM2612 | YM3438 | YMF276 |
//           +--------++--------+--------++--------+---------++--------+--------+--------+
//      aka: |   OPN  ||  OPNA  |  OPN3  ||  OPNB  |  OPNB2  ||  OPN2  |  OPN2C |  OPN2L |
//       FM: |    3   ||    6   |    6   ||    4   |    6    ||    6   |    6   |    6   |
//  AY-8910: |    3   ||    3   |    3   ||    3   |    3    ||    -   |    -   |    -   |
//  ADPCM-A: |    -   ||  6 int |  6 int ||  6 ext |  6 ext  ||    -   |    -   |    -   |
//  ADPCM-B: |    -   ||  1 ext |    -   ||  1 ext |  1 ext  ||    -   |    -   |    -   |
//      DAC: |   no   ||   no   |   no   ||   no   |   no    ||   yes  |   yes  |   yes  |
//   output: | 10.3fp || 16-bit | 16-bit || 16-bit |  16-bit ||  9-bit |  9-bit | 16-bit |
//  summing: |  adder ||  adder |  adder ||  adder |  adder  ||  muxer |  muxer |  adder |
//           +--------++--------+--------++--------+---------++--------+--------+--------+
//
// FM represents the number of FM channels available.
// AY-8910 represents the number of AY-8910-compatible channels that are built in.
// ADPCM-A represents the number of internal/external ADPCM-A channels present.
// ADPCM-B represents the number of internal/external ADPCM-B channels present.
// DAC indicates if a directly-accessible DAC output exists, replacing one channel.
// Output indicates the output format to the final DAC.
// Summing indicates whether channels are added or time divided in the output.
//
// OPL has a similar trove of chip variants:
//
//              +--------+---------++--------++--------++--------++---------+
//     chip ID: | YM3526 |  Y8950  || YM3812 || YM2413 || YMF262 || YMF278B |
//              +--------+---------++--------++--------++--------++---------+
//         aka: |   OPL  |MSX-AUDIO||  OPL2  ||  OPLL  ||  OPL3  ||   OPL4  |
//          FM: |    9   |    9    ||    9   ||    9   ||   18   ||    18   |
//     ADPCM-B: |    -   |  1 ext  ||    -   ||    -   ||    -   ||    -    |
//   wavetable: |    -   |    -    ||    -   ||    -   ||    -   ||    24   |
// instruments: |   no   |    no   ||   no   ||   yes  ||   no   ||    no   |
//      output: | 10.3fp |  10.3fp || 10.3fp ||  9-bit || 16-bit || 16-bit  |
//     summing: |  adder |  adder  ||  adder ||  muxer ||  adder ||  adder  |
//              +--------+---------++--------++--------++--------++---------+
//
// FM represents the number of FM channels available.
// ADPCM-B represents the number of external ADPCM-B channels present.
// Wavetable indicates the number of wavetable channels present.
// Instruments indicates that the chip has built-in instrument selection.
// Output indicates the output format to the final DAC.
// Summing indicates whether channels are added or time divided in the output.
//
// There are several close variants of the YM2413 with different sets of built-
// in instruments. These include the YM2423, YMF281, and DS1001 (aka Konami VRC7).
//
// ===================================================================================
//
// OPN Test Bit Functions (YM2612)
// $21:0: Select which of two unknown signals is read as bit 14 of the test read output.
// $21:1: Some LFO control, unknown function.
// $21:2: Timers increment once every internal clock rather than once every sample. (Untested by me)
// $21:3: Freezes PG. Presumably disables writebacks to the phase register.
// $21:4: Ugly bit. Inverts MSB of operators.
// $21:5: Freezes EG. Presumably disables writebacks to the envelope counter register.
//        Unknown whether this affects the other EG state bits.
// $21:6: Enable reading test data from OPN2 rather than status flags.
// $21:7: Select LSB (1) or MSB (0) of read test data. (Yes, it's backwards.)
// $2C:2 downto 0: Ignored by OPN2, confirmed by die shot.
// $2C:3: Bit 0 of Channel 6 DAC value
// $2C:4: Read 9-bit channel output (1) instead of 14-bit operator output (0)
// $2C:5: Play DAC output over all channels (possibly except for Channel 5--in my testing
//        the DAC is the only thing you hear and it's much louder, you do not get any output
//        from Channel 5; but someone else supposedly found that the pan flags for Channel 5
//        don't affect the panning of this sound, which is only possible if it's not being
//        output during that time slot for some reason. I don't have any other reason to
//        believe this is true though).
// $2C:6: Select function of TEST pin input--both unknown functions.
// $2C:7: Set the TEST pin to be an output (1) instead of input (0).
//




//-------------------------------------------------
//  opl_key_scale_atten - converts an
//  OPL concatenated block (3 bits) and fnum
//  (10 bits) into an attenuation offset; values
//  here are for 6dB/octave, in 0.75dB units
//  (matching total level LSB)
//-------------------------------------------------

inline uint32_t opl_key_scale_atten(uint32_t block, uint32_t fnum_4msb)
{
	// this table uses the top 4 bits of FNUM and are the maximal values
	// (for when block == 7). Values for other blocks can be computed by
	// subtracting 8 for each block below 7.
	static uint8_t const fnum_to_atten[16] = { 0,24,32,37,40,43,45,47,48,50,51,52,53,54,55,56 };
	int32_t result = fnum_to_atten[fnum_4msb] - 8 * (block ^ 7);
	return std::max<int32_t>(0, result);
}



//*********************************************************
//  OPM SPECIFICS
//*********************************************************

#if 0


//*********************************************************
//  OPL SPECIFICS
//*********************************************************

//-------------------------------------------------
//  opl_registers_base - constructor
//-------------------------------------------------

template<int Revision>
opl_registers_base<Revision>::opl_registers_base() :
	m_lfo_am_counter(0),
	m_lfo_pm_counter(0),
	m_noise_lfsr(1),
	m_lfo_am(0)
{
	// create the waveforms
	for (int index = 0; index < WAVEFORM_LENGTH; index++)
		m_waveform[0][index] = abs_sin_attenuation(index) | (bitfield(index, 9) << 15);

	if (WAVEFORMS >= 4)
	{
		uint16_t zeroval = m_waveform[0][0];
		for (int index = 0; index < WAVEFORM_LENGTH; index++)
		{
			m_waveform[1][index] = bitfield(index, 9) ? zeroval : m_waveform[0][index];
			m_waveform[2][index] = m_waveform[0][index] & 0x7fff;
			m_waveform[3][index] = bitfield(index, 8) ? zeroval : (m_waveform[0][index] & 0x7fff);
			if (WAVEFORMS >= 8)
			{
				m_waveform[4][index] = bitfield(index, 9) ? zeroval : m_waveform[0][index * 2];
				m_waveform[5][index] = bitfield(index, 9) ? zeroval : m_waveform[0][(index * 2) & 0x1ff];
				m_waveform[6][index] = bitfield(index, 9) << 15;
				m_waveform[7][index] = (zeroval - m_waveform[0][(index / 2)]) | (bitfield(index, 9) << 15);
			}
		}
	}
}


//-------------------------------------------------
//  save - register for save states
//-------------------------------------------------

template<int Revision>
void opl_registers_base<Revision>::save(fm_interface &intf)
{
	intf.save_item(YMFM_NAME(m_lfo_am_counter));
	intf.save_item(YMFM_NAME(m_lfo_pm_counter));
	intf.save_item(YMFM_NAME(m_lfo_am));
	intf.save_item(YMFM_NAME(m_noise_lfsr));
	intf.save_item(YMFM_NAME(m_regdata));
}


//-------------------------------------------------
//  reset - reset to initial state
//-------------------------------------------------

template<int Revision>
void opl_registers_base<Revision>::reset()
{
	std::fill_n(&m_regdata[0], REGISTERS, 0);
}


//-------------------------------------------------
//  operator_map - return an array of operator
//  indices for each channel; for OPL this is fixed
//-------------------------------------------------

template<int Revision>
void opl_registers_base<Revision>::operator_map(operator_mapping &dest) const
{
	if (Revision <= 2)
	{
		// OPL/OPL2 has a fixed map, all 2 operators
		static const operator_mapping s_fixed_map =
		{ {
			operator_list(  0,  3 ),  // Channel 0 operators
			operator_list(  1,  4 ),  // Channel 1 operators
			operator_list(  2,  5 ),  // Channel 2 operators
			operator_list(  6,  9 ),  // Channel 3 operators
			operator_list(  7, 10 ),  // Channel 4 operators
			operator_list(  8, 11 ),  // Channel 5 operators
			operator_list( 12, 15 ),  // Channel 6 operators
			operator_list( 13, 16 ),  // Channel 7 operators
			operator_list( 14, 17 ),  // Channel 8 operators
		} };
		dest = s_fixed_map;
	}
	else
	{
		// OPL3/OPL4 can be configured for 2 or 4 operators
		uint32_t fourop = fourop_enable();

		dest.chan[ 0] = bitfield(fourop, 0) ? operator_list(  0,  3,  6,  9 ) : operator_list(  0,  3 );
		dest.chan[ 1] = bitfield(fourop, 1) ? operator_list(  1,  4,  7, 10 ) : operator_list(  1,  4 );
		dest.chan[ 2] = bitfield(fourop, 2) ? operator_list(  2,  5,  8, 11 ) : operator_list(  2,  5 );
		dest.chan[ 3] = bitfield(fourop, 0) ? operator_list() : operator_list(  6,  9 );
		dest.chan[ 4] = bitfield(fourop, 1) ? operator_list() : operator_list(  7, 10 );
		dest.chan[ 5] = bitfield(fourop, 2) ? operator_list() : operator_list(  8, 11 );
		dest.chan[ 6] = operator_list( 12, 15 );
		dest.chan[ 7] = operator_list( 13, 16 );
		dest.chan[ 8] = operator_list( 14, 17 );

		dest.chan[ 9] = bitfield(fourop, 3) ? operator_list( 18, 21, 24, 27 ) : operator_list( 18, 21 );
		dest.chan[10] = bitfield(fourop, 4) ? operator_list( 19, 22, 25, 28 ) : operator_list( 19, 22 );
		dest.chan[11] = bitfield(fourop, 5) ? operator_list( 20, 23, 26, 29 ) : operator_list( 20, 23 );
		dest.chan[12] = bitfield(fourop, 3) ? operator_list() : operator_list( 24, 27 );
		dest.chan[13] = bitfield(fourop, 4) ? operator_list() : operator_list( 25, 28 );
		dest.chan[14] = bitfield(fourop, 5) ? operator_list() : operator_list( 26, 29 );
		dest.chan[15] = operator_list( 30, 33 );
		dest.chan[16] = operator_list( 31, 34 );
		dest.chan[17] = operator_list( 32, 35 );
	}
}


//-------------------------------------------------
//  write - handle writes to the register array
//-------------------------------------------------

template<int Revision>
bool opl_registers_base<Revision>::write(uint16_t index, uint8_t data, uint32_t &channel, uint32_t &opmask)
{
	assert(index < REGISTERS);

	// writes to the mode register with high bit set ignore the low bits
	if (index == REG_MODE && bitfield(data, 7) != 0)
		m_regdata[index] |= 0x80;
	else
		m_regdata[index] = data;

	// handle writes to the rhythm keyons
	if (index == 0xbd)
	{
		channel = RHYTHM_CHANNEL;
		opmask = bitfield(data, 5) ? bitfield(data, 0, 5) : 0;
		return true;
	}

	// handle writes to the channel keyons
	if ((index & 0xf0) == 0xb0)
	{
		channel = index & 0x0f;
		if (channel < 9)
		{
			if (IsOpl3Plus)
				channel += 9 * bitfield(index, 8);
			opmask = bitfield(data, 5) ? 15 : 0;
			return true;
		}
	}
	return false;
}


//-------------------------------------------------
//  clock_noise_and_lfo - clock the noise and LFO,
//  handling clock division, depth, and waveform
//  computations
//-------------------------------------------------

static int32_t opl_clock_noise_and_lfo(uint32_t &noise_lfsr, uint16_t &lfo_am_counter, uint16_t &lfo_pm_counter, uint8_t &lfo_am, uint32_t am_depth, uint32_t pm_depth)
{
	// OPL has a 23-bit noise generator for the rhythm section, running at
	// a constant rate, used only for percussion input
	noise_lfsr <<= 1;
	noise_lfsr |= bitfield(noise_lfsr, 23) ^ bitfield(noise_lfsr, 9) ^ bitfield(noise_lfsr, 8) ^ bitfield(noise_lfsr, 1);

	// OPL has two fixed-frequency LFOs, one for AM, one for PM

	// the AM LFO has 210*64 steps; at a nominal 50kHz output,
	// this equates to a period of 50000/(210*64) = 3.72Hz
	uint32_t am_counter = lfo_am_counter++;
	if (am_counter >= 210*64 - 1)
		lfo_am_counter = 0;

	// low 8 bits are fractional; depth 0 is divided by 2, while depth 1 is times 2
	int shift = 9 - 2 * am_depth;

	// AM value is the upper bits of the value, inverted across the midpoint
	// to produce a triangle
	lfo_am = ((am_counter < 105*64) ? am_counter : (210*64+63 - am_counter)) >> shift;

	// the PM LFO has 8192 steps, or a nominal period of 6.1Hz
	uint32_t pm_counter = lfo_pm_counter++;

	// PM LFO is broken into 8 chunks, each lasting 1024 steps; the PM value
	// depends on the upper bits of FNUM, so this value is a fraction and
	// sign to apply to that value, as a 1.3 value
	static int8_t const pm_scale[8] = { 8, 4, 0, -4, -8, -4, 0, 4 };
	return pm_scale[bitfield(pm_counter, 10, 3)] >> (pm_depth ^ 1);
}

template<int Revision>
int32_t opl_registers_base<Revision>::clock_noise_and_lfo()
{
	return opl_clock_noise_and_lfo(m_noise_lfsr, m_lfo_am_counter, m_lfo_pm_counter, m_lfo_am, lfo_am_depth(), lfo_pm_depth());
}


//-------------------------------------------------
//  cache_operator_data - fill the operator cache
//  with prefetched data; note that this code is
//  also used by ymopna_registers, so it must
//  handle upper channels cleanly
//-------------------------------------------------

template<int Revision>
void opl_registers_base<Revision>::cache_operator_data(uint32_t choffs, uint32_t opoffs, opdata_cache &cache)
{
	// set up the easy stuff
	cache.waveform = &m_waveform[op_waveform(opoffs) % WAVEFORMS][0];

	// get frequency from the channel
	uint32_t block_freq = cache.block_freq = ch_block_freq(choffs);

	// compute the keycode: block_freq is:
	//
	//     111  |
	//     21098|76543210
	//     BBBFF|FFFFFFFF
	//     ^^^??
	//
	// the 4-bit keycode uses the top 3 bits plus one of the next two bits
	uint32_t keycode = bitfield(block_freq, 10, 3) << 1;

	// lowest bit is determined by note_select(); note that it is
	// actually reversed from what the manual says, however
	keycode |= bitfield(block_freq, 9 - note_select(), 1);

	// no detune adjustment on OPL
	cache.detune = 0;

	// multiple value, as an x.1 value (0 means 0.5)
	// replace the low bit with a table lookup to give 0,1,2,3,4,5,6,7,8,9,10,10,12,12,15,15
	uint32_t multiple = op_multiple(opoffs);
	cache.multiple = ((multiple & 0xe) | bitfield(0xc2aa, multiple)) * 2;
	if (cache.multiple == 0)
		cache.multiple = 1;

	// phase step, or PHASE_STEP_DYNAMIC if PM is active; this depends on block_freq, detune,
	// and multiple, so compute it after we've done those
	if (op_lfo_pm_enable(opoffs) == 0)
		cache.phase_step = compute_phase_step(choffs, opoffs, cache, 0);
	else
		cache.phase_step = opdata_cache::PHASE_STEP_DYNAMIC;

	// total level, scaled by 8
	cache.total_level = op_total_level(opoffs) << 3;

	// pre-add key scale level
	uint32_t ksl = op_ksl(opoffs);
	if (ksl != 0)
		cache.total_level += opl_key_scale_atten(bitfield(block_freq, 10, 3), bitfield(block_freq, 6, 4)) << ksl;

	// 4-bit sustain level, but 15 means 31 so effectively 5 bits
	cache.eg_sustain = op_sustain_level(opoffs);
	cache.eg_sustain |= (cache.eg_sustain + 1) & 0x10;
	cache.eg_sustain <<= 5;

	// determine KSR adjustment for enevlope rates
	uint32_t ksrval = keycode >> (2 * (op_ksr(opoffs) ^ 1));
	cache.eg_rate[EG_ATTACK] = effective_rate(op_attack_rate(opoffs) * 4, ksrval);
	cache.eg_rate[EG_DECAY] = effective_rate(op_decay_rate(opoffs) * 4, ksrval);
	cache.eg_rate[EG_SUSTAIN] = op_eg_sustain(opoffs) ? 0 : effective_rate(op_release_rate(opoffs) * 4, ksrval);
	cache.eg_rate[EG_RELEASE] = effective_rate(op_release_rate(opoffs) * 4, ksrval);
	cache.eg_shift = 0;
}


//-------------------------------------------------
//  compute_phase_step - compute the phase step
//-------------------------------------------------

static uint32_t opl_compute_phase_step(uint32_t block_freq, uint32_t multiple, int32_t lfo_raw_pm)
{
	// OPL phase calculation has no detuning, but uses FNUMs like
	// the OPN version, and computes PM a bit differently

	// extract frequency number as a 12-bit fraction
	uint32_t fnum = bitfield(block_freq, 0, 10) << 2;

	// apply the phase adjustment based on the upper 3 bits
	// of FNUM and the PM depth parameters
	fnum += (lfo_raw_pm * bitfield(block_freq, 7, 3)) >> 1;

	// keep fnum to 12 bits
	fnum &= 0xfff;

	// apply block shift to compute phase step
	uint32_t block = bitfield(block_freq, 10, 3);
	uint32_t phase_step = (fnum << block) >> 2;

	// apply frequency multiplier (which is cached as an x.1 value)
	return (phase_step * multiple) >> 1;
}

template<int Revision>
uint32_t opl_registers_base<Revision>::compute_phase_step(uint32_t choffs, uint32_t opoffs, opdata_cache const &cache, int32_t lfo_raw_pm)
{
	return opl_compute_phase_step(cache.block_freq, cache.multiple, op_lfo_pm_enable(opoffs) ? lfo_raw_pm : 0);
}


//-------------------------------------------------
//  log_keyon - log a key-on event
//-------------------------------------------------

template<int Revision>
std::string opl_registers_base<Revision>::log_keyon(uint32_t choffs, uint32_t opoffs)
{
	uint32_t chnum = (choffs & 15) + 9 * bitfield(choffs, 8);
	uint32_t opnum = (opoffs & 31) - 2 * ((opoffs & 31) / 8) + 18 * bitfield(opoffs, 8);

	char buffer[256];
	char *end = &buffer[0];

	end += sprintf(end, "%2d.%02d freq=%04X fb=%d alg=%X mul=%X tl=%02X ksr=%d ns=%d ksl=%d adr=%X/%X/%X sl=%X sus=%d",
		chnum, opnum,
		ch_block_freq(choffs),
		ch_feedback(choffs),
		ch_algorithm(choffs),
		op_multiple(opoffs),
		op_total_level(opoffs),
		op_ksr(opoffs),
		note_select(),
		op_ksl(opoffs),
		op_attack_rate(opoffs),
		op_decay_rate(opoffs),
		op_release_rate(opoffs),
		op_sustain_level(opoffs),
		op_eg_sustain(opoffs));

	if (OUTPUTS > 1)
		end += sprintf(end, " out=%c%c%c%c",
			ch_output_0(choffs) ? 'L' : '-',
			ch_output_1(choffs) ? 'R' : '-',
			ch_output_2(choffs) ? '0' : '-',
			ch_output_3(choffs) ? '1' : '-');
	if (op_lfo_am_enable(opoffs) != 0)
		end += sprintf(end, " am=%d", lfo_am_depth());
	if (op_lfo_pm_enable(opoffs) != 0)
		end += sprintf(end, " pm=%d", lfo_pm_depth());
	if (waveform_enable() && op_waveform(opoffs) != 0)
		end += sprintf(end, " wf=%d", op_waveform(opoffs));
	if (is_rhythm(choffs))
		end += sprintf(end, " rhy=1");
	if (DYNAMIC_OPS)
	{
		operator_mapping map;
		operator_map(map);
		if (bitfield(map.chan[chnum], 16, 8) != 0xff)
			end += sprintf(end, " 4op");
	}

	return buffer;
}


//*********************************************************
//  OPLL SPECIFICS
//*********************************************************

//-------------------------------------------------
//  opll_registers - constructor
//-------------------------------------------------

opll_registers::opll_registers() :
	m_lfo_am_counter(0),
	m_lfo_pm_counter(0),
	m_noise_lfsr(1),
	m_lfo_am(0)
{
	// create the waveforms
	for (int index = 0; index < WAVEFORM_LENGTH; index++)
		m_waveform[0][index] = abs_sin_attenuation(index) | (bitfield(index, 9) << 15);

	uint16_t zeroval = m_waveform[0][0];
	for (int index = 0; index < WAVEFORM_LENGTH; index++)
		m_waveform[1][index] = bitfield(index, 9) ? zeroval : m_waveform[0][index];

	// initialize the instruments to something sane
	for (int choffs = 0; choffs < CHANNELS; choffs++)
		m_chinst[choffs] = &m_regdata[0];
	for (int opoffs = 0; opoffs < OPERATORS; opoffs++)
		m_opinst[opoffs] = &m_regdata[bitfield(opoffs, 0)];
}


//-------------------------------------------------
//  save - register for save states
//-------------------------------------------------

void opll_registers::save(fm_interface &intf)
{
	intf.save_item(YMFM_NAME(m_lfo_am_counter));
	intf.save_item(YMFM_NAME(m_lfo_pm_counter));
	intf.save_item(YMFM_NAME(m_lfo_am));
	intf.save_item(YMFM_NAME(m_noise_lfsr));
	intf.save_item(YMFM_NAME(m_regdata));
}


//-------------------------------------------------
//  reset - reset to initial state
//-------------------------------------------------

void opll_registers::reset()
{
	std::fill_n(&m_regdata[0], REGISTERS, 0);
}


//-------------------------------------------------
//  operator_map - return an array of operator
//  indices for each channel; for OPLL this is fixed
//-------------------------------------------------

void opll_registers::operator_map(operator_mapping &dest) const
{
	static const operator_mapping s_fixed_map =
	{ {
		operator_list(  0,  1 ),  // Channel 0 operators
		operator_list(  2,  3 ),  // Channel 1 operators
		operator_list(  4,  5 ),  // Channel 2 operators
		operator_list(  6,  7 ),  // Channel 3 operators
		operator_list(  8,  9 ),  // Channel 4 operators
		operator_list( 10, 11 ),  // Channel 5 operators
		operator_list( 12, 13 ),  // Channel 6 operators
		operator_list( 14, 15 ),  // Channel 7 operators
		operator_list( 16, 17 ),  // Channel 8 operators
	} };
	dest = s_fixed_map;
}


//-------------------------------------------------
//  write - handle writes to the register array;
//  note that this code is also used by
//  ymopl3_registers, so it must handle upper
//  channels cleanly
//-------------------------------------------------

bool opll_registers::write(uint16_t index, uint8_t data, uint32_t &channel, uint32_t &opmask)
{
	// unclear the address is masked down to 6 bits or if writes above
	// the register top are ignored; assuming the latter for now
	if (index >= REGISTERS)
		return false;

	// write the new data
	m_regdata[index] = data;

	// handle writes to the rhythm keyons
	if (index == 0x0e)
	{
		channel = RHYTHM_CHANNEL;
		opmask = bitfield(data, 5) ? bitfield(data, 0, 5) : 0;
		return true;
	}

	// handle writes to the channel keyons
	if ((index & 0xf0) == 0x20)
	{
		channel = index & 0x0f;
		if (channel < CHANNELS)
		{
			opmask = bitfield(data, 4) ? 3 : 0;
			return true;
		}
	}
	return false;
}


//-------------------------------------------------
//  clock_noise_and_lfo - clock the noise and LFO,
//  handling clock division, depth, and waveform
//  computations
//-------------------------------------------------

int32_t opll_registers::clock_noise_and_lfo()
{
	// implementation is the same as OPL with fixed depths
	return opl_clock_noise_and_lfo(m_noise_lfsr, m_lfo_am_counter, m_lfo_pm_counter, m_lfo_am, 1, 1);
}


//-------------------------------------------------
//  cache_operator_data - fill the operator cache
//  with prefetched data; note that this code is
//  also used by ymopna_registers, so it must
//  handle upper channels cleanly
//-------------------------------------------------

void opll_registers::cache_operator_data(uint32_t choffs, uint32_t opoffs, opdata_cache &cache)
{
	// first set up the instrument data
	uint32_t instrument = ch_instrument(choffs);
	if (rhythm_enable() && choffs >= 6)
		m_chinst[choffs] = &m_instdata[8 * (15 + (choffs - 6))];
	else
		m_chinst[choffs] = (instrument == 0) ? &m_regdata[0] : &m_instdata[8 * (instrument - 1)];
	m_opinst[opoffs] = m_chinst[choffs] + bitfield(opoffs, 0);

	// set up the easy stuff
	cache.waveform = &m_waveform[op_waveform(opoffs) % WAVEFORMS][0];

	// get frequency from the channel
	uint32_t block_freq = cache.block_freq = ch_block_freq(choffs);

	// compute the keycode: block_freq is:
	//
	//     11  |
	//     1098|76543210
	//     BBBF|FFFFFFFF
	//     ^^^^
	//
	// the 4-bit keycode uses the top 4 bits
	uint32_t keycode = bitfield(block_freq, 8, 4);

	// no detune adjustment on OPLL
	cache.detune = 0;

	// multiple value, as an x.1 value (0 means 0.5)
	// replace the low bit with a table lookup to give 0,1,2,3,4,5,6,7,8,9,10,10,12,12,15,15
	uint32_t multiple = op_multiple(opoffs);
	cache.multiple = ((multiple & 0xe) | bitfield(0xc2aa, multiple)) * 2;
	if (cache.multiple == 0)
		cache.multiple = 1;

	// phase step, or PHASE_STEP_DYNAMIC if PM is active; this depends on
	// block_freq, detune, and multiple, so compute it after we've done those
	if (op_lfo_pm_enable(opoffs) == 0)
		cache.phase_step = compute_phase_step(choffs, opoffs, cache, 0);
	else
		cache.phase_step = opdata_cache::PHASE_STEP_DYNAMIC;

	// total level, scaled by 8; for non-rhythm operator 0, this is the total
	// level from the instrument data; for other operators it is 4*volume
	if (bitfield(opoffs, 0) == 1 || (rhythm_enable() && choffs >= 7))
		cache.total_level = op_volume(opoffs) * 4;
	else
		cache.total_level = ch_total_level(choffs);
	cache.total_level <<= 3;

	// pre-add key scale level
	uint32_t ksl = op_ksl(opoffs);
	if (ksl != 0)
		cache.total_level += opl_key_scale_atten(bitfield(block_freq, 9, 3), bitfield(block_freq, 5, 4)) << ksl;

	// 4-bit sustain level, but 15 means 31 so effectively 5 bits
	cache.eg_sustain = op_sustain_level(opoffs);
	cache.eg_sustain |= (cache.eg_sustain + 1) & 0x10;
	cache.eg_sustain <<= 5;

	// The envelope diagram in the YM2413 datasheet gives values for these
	// in ms from 0->48dB. The attack/decay tables give values in ms from
	// 0->96dB, so to pick an equivalent decay rate, we want to find the
	// closest match that is 2x the 0->48dB value:
	//
	//     DP =   10ms (0->48db) ->   20ms (0->96db); decay of 12 gives   19.20ms
	//     RR =  310ms (0->48db) ->  620ms (0->96db); decay of  7 gives  613.76ms
	//     RS = 1200ms (0->48db) -> 2400ms (0->96db); decay of  5 gives 2455.04ms
	//
	// The envelope diagram for percussive sounds (eg_sustain() == 0) also uses
	// "RR" to mean both the constant RR above and the Release Rate specified in
	// the instrument data. In this case, Relief Pitcher's credit sound bears out
	// that the Release Rate is used during sustain, and that the constant RR
	// (or RS) is used during the release phase.
	constexpr uint8_t DP = 12 * 4;
	constexpr uint8_t RR = 7 * 4;
	constexpr uint8_t RS = 5 * 4;

	// determine KSR adjustment for envelope rates
	uint32_t ksrval = keycode >> (2 * (op_ksr(opoffs) ^ 1));
	cache.eg_rate[EG_DEPRESS] = DP;
	cache.eg_rate[EG_ATTACK] = effective_rate(op_attack_rate(opoffs) * 4, ksrval);
	cache.eg_rate[EG_DECAY] = effective_rate(op_decay_rate(opoffs) * 4, ksrval);
	if (op_eg_sustain(opoffs))
	{
		cache.eg_rate[EG_SUSTAIN] = 0;
		cache.eg_rate[EG_RELEASE] = ch_sustain(choffs) ? RS : effective_rate(op_release_rate(opoffs) * 4, ksrval);
	}
	else
	{
		cache.eg_rate[EG_SUSTAIN] = effective_rate(op_release_rate(opoffs) * 4, ksrval);
		cache.eg_rate[EG_RELEASE] = ch_sustain(choffs) ? RS : RR;
	}
	cache.eg_shift = 0;
}


//-------------------------------------------------
//  compute_phase_step - compute the phase step
//-------------------------------------------------

uint32_t opll_registers::compute_phase_step(uint32_t choffs, uint32_t opoffs, opdata_cache const &cache, int32_t lfo_raw_pm)
{
	// phase step computation is the same as OPL but the block_freq has one
	// more bit, which we shift in
	return opl_compute_phase_step(cache.block_freq << 1, cache.multiple, op_lfo_pm_enable(opoffs) ? lfo_raw_pm : 0);
}


//-------------------------------------------------
//  log_keyon - log a key-on event
//-------------------------------------------------

std::string opll_registers::log_keyon(uint32_t choffs, uint32_t opoffs)
{
	uint32_t chnum = choffs;
	uint32_t opnum = opoffs;

	char buffer[256];
	char *end = &buffer[0];

	end += sprintf(end, "%d.%02d freq=%04X inst=%X fb=%d mul=%X",
		chnum, opnum,
		ch_block_freq(choffs),
		ch_instrument(choffs),
		ch_feedback(choffs),
		op_multiple(opoffs));

	if (bitfield(opoffs, 0) == 1 || (is_rhythm(choffs) && choffs >= 6))
		end += sprintf(end, " vol=%X", op_volume(opoffs));
	else
		end += sprintf(end, " tl=%02X", ch_total_level(choffs));

	end += sprintf(end, " ksr=%d ksl=%d adr=%X/%X/%X sl=%X sus=%d/%d",
		op_ksr(opoffs),
		op_ksl(opoffs),
		op_attack_rate(opoffs),
		op_decay_rate(opoffs),
		op_release_rate(opoffs),
		op_sustain_level(opoffs),
		op_eg_sustain(opoffs),
		ch_sustain(choffs));

	if (op_lfo_am_enable(opoffs))
		end += sprintf(end, " am=1");
	if (op_lfo_pm_enable(opoffs))
		end += sprintf(end, " pm=1");
	if (op_waveform(opoffs) != 0)
		end += sprintf(end, " wf=1");
	if (is_rhythm(choffs))
		end += sprintf(end, " rhy=1");

	return buffer;
}


//*********************************************************
//  OPQ SPECIFICS
//*********************************************************

//-------------------------------------------------
//  opq_registers - constructor
//-------------------------------------------------

opq_registers::opq_registers() :
	m_lfo_counter(0),
	m_lfo_am(0)
{
	// create the waveforms
	for (int index = 0; index < WAVEFORM_LENGTH; index++)
		m_waveform[0][index] = abs_sin_attenuation(index) | (bitfield(index, 9) << 15);

	uint16_t zeroval = m_waveform[0][0];
	for (int index = 0; index < WAVEFORM_LENGTH; index++)
		m_waveform[1][index] = bitfield(index, 9) ? zeroval : m_waveform[0][index];
}


//-------------------------------------------------
//  save - register for save states
//-------------------------------------------------

void opq_registers::save(fm_interface &intf)
{
	intf.save_item(YMFM_NAME(m_lfo_counter));
	intf.save_item(YMFM_NAME(m_lfo_am));
	intf.save_item(YMFM_NAME(m_regdata));
}


//-------------------------------------------------
//  reset - reset to initial state
//-------------------------------------------------

void opq_registers::reset()
{
	std::fill_n(&m_regdata[0], REGISTERS, 0);

	// enable output on both channels by default
	m_regdata[0x10] = m_regdata[0x11] = m_regdata[0x12] = m_regdata[0x13] = 0xc0;
	m_regdata[0x14] = m_regdata[0x15] = m_regdata[0x16] = m_regdata[0x17] = 0xc0;
}


//-------------------------------------------------
//  operator_map - return an array of operator
//  indices for each channel; for OPM this is fixed
//-------------------------------------------------

void opq_registers::operator_map(operator_mapping &dest) const
{
	// seems like the operators are not swizzled like they are on OPM/OPN?
	static const operator_mapping s_fixed_map =
	{ {
		operator_list(  0,  8, 16, 24 ),  // Channel 0 operators
		operator_list(  1,  9, 17, 25 ),  // Channel 1 operators
		operator_list(  2, 10, 18, 26 ),  // Channel 2 operators
		operator_list(  3, 11, 19, 27 ),  // Channel 3 operators
		operator_list(  4, 12, 20, 28 ),  // Channel 4 operators
		operator_list(  5, 13, 21, 29 ),  // Channel 5 operators
		operator_list(  6, 14, 22, 30 ),  // Channel 6 operators
		operator_list(  7, 15, 23, 31 ),  // Channel 7 operators
	} };
	dest = s_fixed_map;
}


//-------------------------------------------------
//  write - handle writes to the register array
//-------------------------------------------------

bool opq_registers::write(uint16_t index, uint8_t data, uint32_t &channel, uint32_t &opmask)
{
	assert(index < REGISTERS);

	// detune/multiple share a register based on the MSB of what is written
	// remap the multiple values to 100-11F
	if ((index & 0x1f) == 0x40 && bitfield(data, 7))
		index += 0xc0;

	// handle writes to the key on index
	if (index == 0x05)
	{
		channel = bitfield(data, 0, 3);
		opmask = bitfield(data, 3, 4);
		return true;
	}
	return false;
}


//-------------------------------------------------
//  clock_noise_and_lfo - clock the noise and LFO,
//  handling clock division, depth, and waveform
//  computations
//-------------------------------------------------

int32_t opq_registers::clock_noise_and_lfo()
{
	// OPQ LFO is not well-understood, but the enable and rate values
	// look a lot like OPN, so we'll crib from there as a starting point

	// if LFO not enabled (not present on OPN), quick exit with 0s
	if (!lfo_enable())
	{
		m_lfo_counter = 0;
		m_lfo_am = 0;
		return 0;
	}

	// this table is based on converting the frequencies in the applications
	// manual to clock dividers, based on the assumption of a 7-bit LFO value
	static uint8_t const lfo_max_count[8] = { 109, 78, 72, 68, 63, 45, 9, 6 };
	uint32_t subcount = uint8_t(m_lfo_counter++);

	// when we cross the divider count, add enough to zero it and cause an
	// increment at bit 8; the 7-bit value lives from bits 8-14
	if (subcount >= lfo_max_count[lfo_rate()])
		m_lfo_counter += subcount ^ 0xff;

	// AM value is 7 bits, staring at bit 8; grab the low 6 directly
	m_lfo_am = bitfield(m_lfo_counter, 8, 6);

	// first half of the AM period (bit 6 == 0) is inverted
	if (bitfield(m_lfo_counter, 8+6) == 0)
		m_lfo_am ^= 0x3f;

	// PM value is 5 bits, starting at bit 10; grab the low 3 directly
	int32_t pm = bitfield(m_lfo_counter, 10, 3);

	// PM is reflected based on bit 3
	if (bitfield(m_lfo_counter, 10+3))
		pm ^= 7;

	// PM is negated based on bit 4
	return bitfield(m_lfo_counter, 10+4) ? -pm : pm;
}


//-------------------------------------------------
//  lfo_am_offset - return the AM offset from LFO
//  for the given channel
//-------------------------------------------------

uint32_t opq_registers::lfo_am_offset(uint32_t choffs) const
{
	// OPM maps AM quite differently from OPN

	// shift value for AM sensitivity is [*, 0, 1, 2],
	// mapping to values of [0, 23.9, 47.8, and 95.6dB]
	uint32_t am_sensitivity = ch_lfo_am_sens(choffs);
	if (am_sensitivity == 0)
		return 0;

	// QUESTION: see OPN note below for the dB range mapping; it applies
	// here as well

	// raw LFO AM value on OPM is 0-FF, which is already a factor of 2
	// larger than the OPN below, putting our staring point at 2x theirs;
	// this works out since our minimum is 2x their maximum
	return m_lfo_am << (am_sensitivity - 1);
}


//-------------------------------------------------
//  cache_operator_data - fill the operator cache
//  with prefetched data
//-------------------------------------------------

void opq_registers::cache_operator_data(uint32_t choffs, uint32_t opoffs, opdata_cache &cache)
{
	// set up the easy stuff
	cache.waveform = &m_waveform[op_waveform(opoffs)][0];

	// get frequency from the appropriate registers
	uint32_t block_freq = cache.block_freq = (opoffs & 1) ? ch_block_freq_24(choffs) : ch_block_freq_13(choffs);

	// compute the keycode: block_freq is:
	//
	//     BBBFFFFFFFFFFFF
	//     ^^^^???
	//
	// keycode is not understood, so just guessing it is like OPN:
	// the 5-bit keycode uses the top 4 bits plus a magic formula
	// for the final bit
	uint32_t keycode = bitfield(block_freq, 11, 4) << 1;

	// lowest bit is determined by a mix of next lower FNUM bits
	// according to this equation from the YM2608 manual:
	//
	//   (F11 & (F10 | F9 | F8)) | (!F11 & F10 & F9 & F8)
	//
	// for speed, we just look it up in a 16-bit constant
	keycode |= bitfield(0xfe80, bitfield(block_freq, 8, 4));

	// detune adjustment; detune isn't really understood except that it is a
	// 6-bit value where the middle value (0x20) means no detune; range is +/-20 cents
	// this calculation gives a bit more, but shifting by 12 gives a bit less
	// also, the real calculation is probably something to do with keycodes
	cache.detune = ((op_detune(opoffs) - 0x20) * block_freq) >> 11;

	// multiple value, as an x.1 value (0 means 0.5)
	static const uint8_t s_multiple_map[16] = { 1,2,4,6,8,10,12,14,16,18,20,24,30,32,34,36 };
	cache.multiple = s_multiple_map[op_multiple(opoffs)];

	// phase step, or PHASE_STEP_DYNAMIC if PM is active; this depends on
	// block_freq, detune, and multiple, so compute it after we've done those
	if (lfo_enable() == 0 || ch_lfo_pm_sens(choffs) == 0)
		cache.phase_step = compute_phase_step(choffs, opoffs, cache, 0);
	else
		cache.phase_step = opdata_cache::PHASE_STEP_DYNAMIC;

	// total level, scaled by 8
	cache.total_level = op_total_level(opoffs) << 3;

	// 4-bit sustain level, but 15 means 31 so effectively 5 bits
	cache.eg_sustain = op_sustain_level(opoffs);
	cache.eg_sustain |= (cache.eg_sustain + 1) & 0x10;
	cache.eg_sustain <<= 5;

	// determine KSR adjustment for enevlope rates; KSR is supposedly 3 bits
	// not 2 like all other implementations, so unsure how this would work.
	// Maybe keycode is a larger range? For now, we'll just take the upper 2
	// bits and use that.
	uint32_t ksrval = keycode >> ((op_ksr(opoffs) >> 1) ^ 3);
	cache.eg_rate[EG_ATTACK] = effective_rate(op_attack_rate(opoffs) * 2, ksrval);
	cache.eg_rate[EG_DECAY] = effective_rate(op_decay_rate(opoffs) * 2, ksrval);
	cache.eg_rate[EG_SUSTAIN] = effective_rate(op_sustain_rate(opoffs) * 2, ksrval);
	cache.eg_rate[EG_RELEASE] = effective_rate(op_release_rate(opoffs) * 4 + 2, ksrval);
	cache.eg_shift = 0;
}


//-------------------------------------------------
//  compute_phase_step - compute the phase step
//-------------------------------------------------

uint32_t opq_registers::compute_phase_step(uint32_t choffs, uint32_t opoffs, opdata_cache const &cache, int32_t lfo_raw_pm)
{
	// OPN phase calculation has only a single detune parameter
	// and uses FNUMs instead of keycodes

	// extract frequency number (low 12 bits of block_freq)
	uint32_t fnum = bitfield(cache.block_freq, 0, 12);

	// if there's a non-zero PM sensitivity, compute the adjustment
	uint32_t pm_sensitivity = ch_lfo_pm_sens(choffs);
	if (pm_sensitivity != 0)
	{
		// apply the phase adjustment based on the upper 7 bits
		// of FNUM and the PM depth parameters
		fnum += opn_lfo_pm_phase_adjustment(bitfield(cache.block_freq, 5, 7), pm_sensitivity, lfo_raw_pm);

		// keep fnum to 12 bits
		fnum &= 0xfff;
	}

	// this is likely not right, but should given the right approximate result
	fnum += cache.detune;

	// apply block shift to compute phase step
	uint32_t block = bitfield(cache.block_freq, 12, 3);
	uint32_t phase_step = (fnum << block) >> 2;

	// apply detune based on the keycode -- this is probably where the real chip does it
//	phase_step += cache.detune;

	// clamp to 17 bits in case detune overflows
	// QUESTION: is this specific to the YM2612/3438?
	phase_step &= 0x1ffff;

	// apply frequency multiplier (which is cached as an x.1 value)
	return (phase_step * cache.multiple) >> 1;
}


//-------------------------------------------------
//  log_keyon - log a key-on event
//-------------------------------------------------

std::string opq_registers::log_keyon(uint32_t choffs, uint32_t opoffs)
{
	uint32_t chnum = choffs;
	uint32_t opnum = opoffs;

	char buffer[256];
	char *end = &buffer[0];

	end += sprintf(end, "%d.%02d freq=%04X dt=%d fb=%d alg=%X mul=%X tl=%02X ksr=%d adsr=%02X/%02X/%02X/%X sl=%X out=%c%c",
		chnum, opnum,
		(opoffs & 1) ? ch_block_freq_24(choffs) : ch_block_freq_13(choffs),
		op_detune(opoffs),
		ch_feedback(choffs),
		ch_algorithm(choffs),
		op_multiple(opoffs),
		op_total_level(opoffs),
		op_ksr(opoffs),
		op_attack_rate(opoffs),
		op_decay_rate(opoffs),
		op_sustain_rate(opoffs),
		op_release_rate(opoffs),
		op_sustain_level(opoffs),
		ch_output_0(choffs) ? 'L' : '-',
		ch_output_1(choffs) ? 'R' : '-');

	bool am = (lfo_enable() && op_lfo_am_enable(opoffs) && ch_lfo_am_sens(choffs) != 0);
	if (am)
		end += sprintf(end, " am=%d", ch_lfo_am_sens(choffs));
	bool pm = (lfo_enable() && ch_lfo_pm_sens(choffs) != 0);
	if (pm)
		end += sprintf(end, " pm=%d", ch_lfo_pm_sens(choffs));
	if (am || pm)
		end += sprintf(end, " lfo=%02X", lfo_rate());
	if (ch_echo(choffs))
		end += sprintf(end, " echo");

	return buffer;
}
#endif
}
