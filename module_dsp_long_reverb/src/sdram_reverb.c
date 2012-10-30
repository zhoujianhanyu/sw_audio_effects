/******************************************************************************\
 * File:	sdram_reverb.c
 *  
 * Description: Control coar for Reverb, also handles delay functionality, 
 *	calls Loudness and Equalisation coars
 *
 * Version: 0v1
 * Build:
 *
 * The copyrights, all other intellectual and industrial
 * property rights are retained by XMOS and/or its licensors.
 * Terms and conditions covering the use of this code can
 * be found in the Xmos End User License Agreement.
 *
 * Copyright XMOS Ltd 2012
 *
 * In the case where this code is a modification of existing code
 * under a separate license, the separate license terms are shown
 * below. The modifications to the code are still covered by the
 * copyright notice above.
 *
\******************************************************************************/

#include "sdram_reverb.h"

// Structure containing all reverb data (NB Make global to avoid using malloc)
static REVERB_S reverb_gs = { .ratios = {{ TAP_00 ,TAP_01 ,TAP_02 ,TAP_04 ,TAP_05 ,TAP_06 ,TAP_07 
																					,TAP_08 ,TAP_09 ,TAP_10 ,TAP_11 ,TAP_13 ,TAP_14 ,TAP_15
																				}}
                              ,.init_done = 0
															,.params_set = 0
														};

/*****************************************************************************/
void config_build_delay( // Calculate delay parameters and call delay configuration
	REVERB_S * reverb_ps, // Pointer to structure containing reverb data
	REVERB_PARAM_S * reverb_param_ps // Pointer to structure containing reverb parameters
)
{
	DELAY_PARAM_S delay_param_s =  { .num = NUM_REVERB_TAPS }; // Default Delay-line Configuration
	U64_T factor; // scaling factor for delay taps
	S32_T tap_cnt; // delay measured in samples


	delay_param_s.freq = reverb_param_ps->samp_freq; // Assign requested sample frequency
	
	// Calculate Delay-line taps (in milli-seconds) for requested room-size ...

	factor = (S64_T)1000000 * (S64_T)INV_SOS * (S64_T)reverb_param_ps->room_size; // common factor to convert delay to milli-seconds

	// Loop through all delay taps.
	for (tap_cnt = 0; tap_cnt < NUM_REVERB_TAPS; tap_cnt++)
	{
		// Calculate delay taps in samples.
		delay_param_s.us_delays[tap_cnt] 
			= (S32_T)((factor * (S64_T)reverb_ps->ratios.taps[tap_cnt] + HALF_REV_SCALE ) >> SCALE_REV_BITS);
	} // for tap_cnt

	config_sdram_delay( &(delay_param_s) );

} // config_build_delay
/*****************************************************************************/
void init_reverb( // Initialise reverb parameters
	REVERB_S * reverb_ps // Pointer to structure containing reverb data
)
{
	assert(14 == NUM_REVERB_TAPS); // ERROR: Only NUM_REVERB_TAPS=16 supported
	assert((MAX_TAP + 1) == reverb_ps->ratios.taps[1]); // Calculations depend 2nd tap value of approx MAX_TAP
} // init_reverb
/*****************************************************************************/
void config_sdram_reverb( // Configure reverb parameters
	REVERB_PARAM_S * reverb_param_ps // Pointer to structure containing reverb parameters
)
{
	// Check if reverb initialised
	if (0 == reverb_gs.init_done)
	{ // Initialse Reverb
		init_reverb( &(reverb_gs) );

		reverb_gs.init_done = 1; // Signal Reverb initialised
	} // if (0 == reverb_gs->init_done)

	assert(MIN_AUDIO_FREQ <= reverb_param_ps->samp_freq); // Check for sensible frequency

	/* Configure Delay-line which is in this coar. 
		NB BiQuad and Loudness are in different coars and therefore configuration is synchronised via dsp_sdram_reverb.xc */

	config_build_delay( &(reverb_gs) ,reverb_param_ps );  

	// Assign requested mix setting ...

	reverb_gs.mix_lvls_ps = &(reverb_param_ps->mix_lvls);

	reverb_gs.params_set = 1; // Signal Reverb parameters configured

} // config_sdram_reverb
/*****************************************************************************/
void use_sdram_reverb( // Controls audio stream processing for reverb application using dsp functions
	CNTRL_SDRAM_S * cntrl_ps, // Pointer to structure containing data to control SDRAM buffering
	CHAN_SET_S * inp_set_ps,	// Pointer to structure containing Input audio sample-set
	CHAN_SET_S * uneq_o_set_ps,	// Pointer to structure containing Unequalised audio sample-set
	CHAN_SET_S * rev_o_set_ps,	// Pointer to structure containing output audio sample-set
	CHAN_SET_S * out_set_ps,	// Pointer to structure containing output audio sample-set
	CHAN_SET_S * ampli_i_set_ps	// Pointer to structure containing Amplified audio sample-set
)
{
	CHAN_SET_S * delay_set_ps = &(cntrl_ps->delay_sets[0]);	// Local ptr to array of structs containing delayed audio sample-sets
	MIX_PARAM_S * mix_lvls_ps = reverb_gs.mix_lvls_ps; // Local Pointer to structure containing mix-levels
	S64_T same_samps[NUM_REVERB_CHANS];	// Same-channel buffer for Channel-Mix (E.g. left->left)
	S64_T swap_samps[NUM_REVERB_CHANS];	// Swap-channel buffer for Channel-Mix (E.g. left->right)
  S64_T sum_same; // Sums output delay-taps for same-channel
  S64_T sum_swap; // Sums output delay-taps for swap-channel
	S64_T samp_sum;	// Intermediate sample sum
	S64_T samp_diff;	// Intermediate sample difference
	S32_T chan_cnt; // Channel counter
	S32_T swap_chan; // Other channel of stereo pair


	// Check if reverb parameters have been initialised
	if (0 == reverb_gs.params_set)
	{
		assert(0 == 1); // Please call config_reverb() function before use_reverb() 
	} // if (0 == reverb_gs.params_set)
	else
	{
		use_sdram_delay( cntrl_ps ); // Get Delayed Samples

		/* We have 16 delay-taps that have to be summed into left and right output channels, 
		 * the shorter delays will be louder than the long delays. 
     * Therefore the shorter delays are given a larger weighting than the larger delays.
     * Some delays are output on the SAME channel that they originated, 
     * and the others are SWAPed to the other output channel. 
     * The table below shows the channel assignments for each tap.
		 *
		 * Tap_Num:   0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15
		 * Out_Chan:  L   L   R   L   L   L   R   R   R   R   L   L   R   R   L   R
		 *
		 * NB. Due to lack of Memory bandwidth currnetly only 14 taps are used.
		 *
		 * Tap_Num:   0   1   2   .   3   4   5   6   7   8   9  10   .  11  12  13
		 * Out_Chan:  L   L   R   .   L   L   R   R   R   R   L   L   .   R   L   R
		 * Weight:   127 125 119  .  113 109 109 101 97  99  89  91   .  81  79  75
		 *
		 * NB. When changing number of taps in use. All weights below have to be recalculated to ensure sums are normalised correctly.
		 */

		// Loop through set of channel samples and process delay taps
		for(chan_cnt = 0; chan_cnt < NUM_REVERB_CHANS; chan_cnt++)
		{
			// NB Equalisation coar converts uneq_o_set to cntrl_ps->src_set.samps
// cntrl_ps->src_set.samps[chan_cnt] = uneq_o_set_ps->samps[chan_cnt]; // MB~ Dbg Switch off Equalisation

			sum_same = 127 * (S64_T)delay_set_ps[0].samps[chan_cnt];
			sum_same += 125 * (S64_T)delay_set_ps[1].samps[chan_cnt];
			sum_same += 115 * (S64_T)delay_set_ps[3].samps[chan_cnt];
			sum_same += 108 * (S64_T)delay_set_ps[4].samps[chan_cnt];
			sum_same += 89 * (S64_T)delay_set_ps[9].samps[chan_cnt];
			sum_same += 90 * (S64_T)delay_set_ps[10].samps[chan_cnt];
			sum_same += 80 * (S64_T)delay_set_ps[12].samps[chan_cnt];

			// Sum weighted pre-delay taps for use in swap output channel.
			sum_swap = 118 * (S64_T)delay_set_ps[2].samps[chan_cnt];
			sum_swap += 109 * (S64_T)delay_set_ps[5].samps[chan_cnt];
			sum_swap += 102 * (S64_T)delay_set_ps[6].samps[chan_cnt];
			sum_swap += 95 * (S64_T)delay_set_ps[7].samps[chan_cnt];
			sum_swap += 96 * (S64_T)delay_set_ps[8].samps[chan_cnt];
			sum_swap += 83 * (S64_T)delay_set_ps[11].samps[chan_cnt];
			sum_swap += 77 * (S64_T)delay_set_ps[13].samps[chan_cnt];

			// Attenuate earliest pre-delay to 1/4 volume by dividing by 512 
			same_samps[chan_cnt] = (sum_same + 256 ) >> 9;
			swap_samps[chan_cnt] = (sum_swap + 256 ) >> 9;
// same_samps[chan_cnt] = cntrl_ps->src_set.samps[chan_cnt]; //MB~ Dbg Switch off Pre-Delay
// swap_samps[chan_cnt] = cntrl_ps->src_set.samps[chan_cnt]; //MB~ Dbg Switch off Pre-Delay

			/* Above weight-sum is currently 14*101 = 1414.
			 * To remove the requirement for a divide, 1414 is approximated to 2^21 / 1483.
			 */

			// Sum pre-delays for feeding back into delay-line
			samp_sum = (S32_T)((1483 * (sum_same + sum_swap) + (2 << 20)) >> 21); // Approximate normalised value

			// Control Reverb Feedback/Attenuation (User Controlable)
			samp_sum = ((samp_sum * (S64_T)mix_lvls_ps->fb_lvl) + (S64_T)MIX_DIV2) >> MIX_BITS; // Set feedback level
			uneq_o_set_ps->samps[chan_cnt] = (S32_T)(samp_sum + (S64_T)(inp_set_ps->samps[chan_cnt] >> 1)); // Add in half of dry signal
// uneq_o_set_ps->samps[chan_cnt] = inp_set_ps->samps[chan_cnt]; //MB~ Dbg Switch-off Feedback
		} // for chan_cnt

		// NB Requires two loops, as left and right channel need to have been updated before mixing can be done
		// Loop through set of channel samples and produce Left/Right output mixes
		for(chan_cnt = 0; chan_cnt < NUM_REVERB_CHANS; chan_cnt++)
		{
			swap_chan = chan_cnt ^ 1; // Get Id for other channel (Assumes 0 <--> 1, 2 <--> 3, etc)

			// Mix Left and Right channels 
			samp_diff = (S64_T)mix_lvls_ps->cross_mix * (swap_samps[swap_chan] - same_samps[chan_cnt]);
			rev_o_set_ps->samps[chan_cnt] = (S32_T)(same_samps[chan_cnt] + ((samp_diff + (S64_T)MIX_DIV2) >> MIX_BITS));
// rev_o_set_ps->samps[chan_cnt] = same_samps[chan_cnt]; //MB~ Dbg Switch to Mono-Channel mix

			// NB Non-linear-gain coar converts rev_o_set_s to ampli_i_set_s
// ampli_i_set_ps->samps[chan_cnt] = rev_o_set_ps->samps[chan_cnt]; // MB~ Dbg Switch off Gain

			// Mix Dry and Reverb Signals (User Controlable)
			samp_sum = (S64_T)mix_lvls_ps->dry_lvl * (S64_T)inp_set_ps->samps[chan_cnt]; // Initialise with Dry signal level
			samp_sum += (S64_T)mix_lvls_ps->fx_lvl * (S64_T)ampli_i_set_ps->samps[chan_cnt]; // Add-in Effect signal level
			out_set_ps->samps[chan_cnt] = (S32_T)((samp_sum + (S64_T)MIX_DIV2) >> MIX_BITS);
// out_set_ps->samps[chan_cnt] = ampli_i_set_ps->samps[chan_cnt]; // MB~ Dbg Switch to all effect mix
		} // for chan_cnt
	} // else !(0 == reverb_gs.params_set)

} // use_sdram_reverb
/*****************************************************************************/
// sdram_reverb.c
