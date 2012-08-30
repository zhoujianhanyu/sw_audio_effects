/******************************************************************************\
 * File:	dsp_loudness.xc
 * Author: Mark Beaumont
 * Description: Thread that applies non-linear gain to stream of audio samples
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

#include "dsp_loudness.h"

// DSP-control thread.

/******************************************************************************/
void dsp_loudness( // Thread that applies non-linear gain control to stream of audio samples
	streaming chanend c_dsp_gain // Channel connecting to DSP-control thread (bi-directional)
)
{
	S32_T unamp_samps[I2S_CHANS_ADC];	// Unamplified audio sample buffer
	S32_T ampli_samps[I2S_CHANS_DAC];	// Amplified audio sample buffer

	S32_T min_chans = I2S_CHANS_DAC;	// Preset minimum of input/output channels to No. of Output channels
	S32_T chan_cnt; // Channel counter
	int num_iters = 4; // Number of Iterations of loudness algorithm
	

	// if necessary, update minimum number of channels
	if (min_chans > I2S_CHANS_ADC)
	{
		min_chans = I2S_CHANS_ADC;
	} // if (min_chans > I2S_CHANS_ADC)

	// initialise samples buffers
	for (chan_cnt = 0; chan_cnt < min_chans; chan_cnt++)
	{
		unamp_samps[chan_cnt] = 0;
		ampli_samps[chan_cnt] = 0;
	}

	// Loop forever
	while(1)
	{ 
		// Send/Receive samples over Audio thread channel
#pragma loop unroll
		for (chan_cnt = 0; chan_cnt < min_chans; chan_cnt++)
		{
			c_dsp_gain :> unamp_samps[chan_cnt]; 
			c_dsp_gain <: ampli_samps[chan_cnt]; 
		}

		// Loop through set of channel samples and process sudio
		for(chan_cnt = 0; chan_cnt < min_chans; chan_cnt++)
		{ // Apply non-linear gain shaping (Loudness)
			ampli_samps[chan_cnt] = non_linear_gain_wrapper( unamp_samps[chan_cnt] ,chan_cnt ,num_iters );
//			ampli_samps[chan_cnt] = unamp_samps[chan_cnt]; // DBG
		}
	}
} // dsp_loudness
/*****************************************************************************/
// dsp_loudness.xc
