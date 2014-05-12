/*
 * Copyright 2011 Free Software Foundation, Inc.
 *
 * This file is part of GNU Radio
 *
 * GNU Radio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * GNU Radio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */


/*
 * GNU Radio C++ example creating dial tone
 * ("the simplest thing that could possibly work")
 *
 * Send a tone each to the left and right channels of stereo audio
 * output and let the user's brain sum them.
 *
 * GNU Radio makes extensive use of Boost shared pointers.  Signal processing
 * blocks are typically created by calling a "make" factory function, which
 * returns an instance of the block as a typedef'd shared pointer that can
 * be used in any way a regular pointer can.  Shared pointers created this way
 * keep track of their memory and free it at the right time, so the user
 * doesn't need to worry about it (really).
 *
 */

// Include header files for each block used in flowgraph

#include <iostream>
#include <fstream> 
#include <string> 
#include <algorithm>    // copy
#include <iterator> 
#include <cstddef>

#include "logging_receiver_dsd.h"
#include "smartnet_crc.h"
#include "smartnet_deinterleave.h"
#include "talkgroup.h"

#include <osmosdr_source_c.h>
#include <osmosdr_sink_c.h>

#include <boost/program_options.hpp>
#include <boost/math/constants/constants.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/tokenizer.hpp>


#include <filter/freq_xlating_fir_filter_ccf.h>
#include <filter/firdes.h>

#include <digital_fll_band_edge_cc.h>
#include <digital_clock_recovery_mm_ff.h>
#include <digital_binary_slicer_fb.h>

#include <gr_firdes.h>
#include <gr_fir_filter_ccf.h>

#include <gr_pll_freqdet_cf.h>
#include <gr_sig_source_f.h>
#include <gr_sig_source_c.h>
#include <gr_audio_sink.h>
#include <gr_correlate_access_code_tag_bb.h>
#include <gr_msg_queue.h>
#include <gr_message.h>
#include <gr_file_sink.h>
 #include <gr_null_sink.h>
#include <gr_complex.h>
#include <gr_fir_filter_ccf.h>
 #include <gr_top_block.h>
#include <gr_multiply_cc.h>

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <ncurses.h>
#include <menu.h>



namespace po = boost::program_options;

using namespace std;


int lastcmd = 0;
int thread_num=0;
double center_freq;

gr_top_block_sptr tb;
osmosdr_source_c_sptr src;


vector<log_dsd_sptr> loggers;
unsigned int max_loggers = 1;

vector<log_dsd_sptr> active_loggers;

vector<Talkgroup *> talkgroups;
vector<Talkgroup *> active_tg;
char **menu_choices;
char status[150];
ITEM **tg_menu_items;


WINDOW *active_tg_win;
WINDOW *tg_menu_win;
WINDOW *status_win;
MENU *tg_menu;



volatile sig_atomic_t exit_flag = 0;




void exit_interupt(int sig){ // can be called asynchronously
  exit_flag = 1; // set flag
}

void init_loggers(int num, float center_freq) {

// static loggers
	for (int i = 0; i < num; i++) {
		log_dsd_sptr log = make_log_dsd( center_freq, center_freq, 0, i);			
		loggers.push_back(log);
		tb->connect(src, 0, log, 0);

 	}

}

float getfreq(int cmd) {
	float freq;
		if (cmd < 0x1b8) {	
			freq = float(cmd * 0.025 + 851.0125);
		} else if (cmd < 0x230) {
			freq = float(cmd * 0.025 + 851.0125 - 10.9875);
			} else {
			freq = 0;
			}
	
	return freq;
}



float parse_message(string s) {
	float retfreq = 0;
	bool rxfound = false;
	std::vector<std::string> x;
	boost::split(x, s, boost::is_any_of(","), boost::token_compress_on);

	int address = atoi( x[0].c_str() ) & 0xFFF0;
	int groupflag = atoi( x[1].c_str() );
	int command = atoi( x[2].c_str() );
	char shell_command[200];
	
	x.clear();
	vector<string>().swap(x);

        if (command < 0x2d0) {

		if ( lastcmd == 0x308) {
		        // Channel Grant
			if (  (address != 56016) && (address != 8176)) {
				retfreq = getfreq(command);
			}
		} else {
			// Call continuation
			if  ( (address != 56016) && (address != 8176))  { 
				retfreq = getfreq(command);
			}
		}
	}

	if (command == 0x03c0) {
		//parse_status(command, address,groupflag);
	}


	if (retfreq) {
		for(vector<log_dsd_sptr>::iterator it = active_loggers.begin(); it != active_loggers.end(); ++it) {	
			log_dsd_sptr rx = *it;	
						
			if (rx->get_talkgroup() == address) {		
				if (rx->get_freq() != retfreq) {

					rx->tune_offset(retfreq);
				}
				rx->unmute();
				
				rxfound = true;
			} else {
				if (rx->get_freq() == retfreq) {
					//cout << "  !! Someone else is on my Channel - My TG: "<< rx->get_talkgroup() << " Freq: " <<rx->get_freq() << " Intruding TG: " << address << endl;
					rx->mute();
				}
			}
		}


		if ((!rxfound)){ 
		  	bool record_tg = false;
				if (active_loggers.size() < max_loggers) {
				    record_tg = true;
				    
				  } else {
				  record_tg = false;
				  }
				

		  if (record_tg){
			log_dsd_sptr log = loggers.front();
			active_loggers.push_back(move(log));
			loggers.erase(loggers.begin());
			log->activate(retfreq, address,active_loggers.size());
		  }			
		}
		
	}


	for(vector<log_dsd_sptr>::iterator it = active_loggers.begin(); it != active_loggers.end();) {
		log_dsd_sptr rx = *it;

		if (rx->lastupdate() > 5.0) {
			


			rx->deactivate();

			sprintf(shell_command,"./encode-upload.sh %s > /dev/null 2>&1 &", rx->get_filename());
			system(shell_command);

			//static loggers
			loggers.push_back(move(rx));
			

			it = active_loggers.erase(it);
			

		} else {
			++it;
		}
	}

	lastcmd = command;
       

	return retfreq;
}


int main(int argc, char **argv)
{

std::string device_addr;
    double  samp_rate, chan_freq, error;
	int if_gain, bb_gain, rf_gain;
    //setup the program options
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help", "help message")
        ("arg", po::value<std::string>(&device_addr)->default_value(""), "the device arguments in string format")
        ("rate", po::value<double>(&samp_rate)->default_value(1e6), "the sample rate in samples per second")
        ("center", po::value<double>(&center_freq)->default_value(10e6), "the center frequency in Hz")
	("error", po::value<double>(&error)->default_value(0), "the Error in frequency in Hz")
	("freq", po::value<double>(&chan_freq)->default_value(10e6), "the frequency in Hz of the trunking channel")
        ("rfgain", po::value<int>(&rf_gain)->default_value(14), "RF Gain")
	("bbgain", po::value<int>(&bb_gain)->default_value(25), "BB Gain")
	("ifgain", po::value<int>(&if_gain)->default_value(25), "IF Gain")
    ;
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    //print the help message
    if (vm.count("help")){
        std::cout
            << boost::format("SmartNet Trunking Reciever %s") % desc << std::endl
            << "The tags sink demo block will print USRP source time stamps." << std::endl
            << "The tags source demo block will send bursts to the USRP sink." << std::endl
            << "Look at the USRP output on a scope to see the timed bursts." << std::endl
            << std::endl;
        return ~0;
    }




 	signal(SIGINT, exit_interupt);


 	tb = gr_make_top_block("smartnet");

	
	src = osmosdr_make_source_c();
	cout << "Setting sample rate to: " << samp_rate << endl;
	src->set_sample_rate(samp_rate);
	cout << "Tunning to " << center_freq - error << "hz" << endl;
	src->set_center_freq(center_freq - error,0);

	cout << "Setting RF gain to " << rf_gain << endl;
	cout << "Setting BB gain to " << bb_gain << endl;
	cout << "Setting IF gain to " << if_gain << endl;

	src->set_gain(rf_gain);
	src->set_if_gain(if_gain);
	src->set_bb_gain(bb_gain);




	float samples_per_second = samp_rate;
	float syms_per_sec = 3600;
	float gain_mu = 0.01;
	float mu=0.5;
	float omega_relative_limit = 0.3;
	float offset = center_freq - chan_freq;
	float clockrec_oversample = 3;
	int decim = int(samples_per_second / (syms_per_sec * clockrec_oversample));
	float sps = samples_per_second/decim/syms_per_sec; 
	const double pi = boost::math::constants::pi<double>();
	
	cout << "Control channel offset: " << offset << endl;
	cout << "Decim: " << decim << endl;
	cout << "Samples per symbol: " << sps << endl;


	init_loggers(max_loggers, center_freq);

	gr_msg_queue_sptr queue = gr_make_msg_queue();


	gr_freq_xlating_fir_filter_ccf_sptr prefilter = gr_make_freq_xlating_fir_filter_ccf(decim, 
						       gr_firdes::low_pass(1, samp_rate, 10000, 12000),
						       offset, 
						       samp_rate);


	gr_pll_freqdet_cf_sptr pll_demod = gr_make_pll_freqdet_cf(2.0 / clockrec_oversample, 										 2*pi/clockrec_oversample, 
										-2*pi/clockrec_oversample);

	digital_fll_band_edge_cc_sptr carriertrack = digital_make_fll_band_edge_cc(sps, 0.6, 64, 0.35);

	digital_clock_recovery_mm_ff_sptr softbits = digital_make_clock_recovery_mm_ff(sps, 0.25 * gain_mu * gain_mu, mu, gain_mu, omega_relative_limit); 


	digital_binary_slicer_fb_sptr slicer =  digital_make_binary_slicer_fb();
	gr_correlate_access_code_tag_bb_sptr start_correlator = gr_make_correlate_access_code_tag_bb("10101100",0,"smartnet_preamble");


	smartnet_deinterleave_sptr deinterleave = smartnet_make_deinterleave();

	smartnet_crc_sptr crc = smartnet_make_crc(queue);

	/*	gr_null_sink_sptr nullsink = gr_make_null_sink(sizeof(u_char));
	tb->connect(deinterleave,0,nullsink,0);*/	
	
	tb->connect(src,0,prefilter,0);
	tb->connect(prefilter,0,carriertrack,0);
	tb->connect(carriertrack, 0, pll_demod, 0);
	tb->connect(pll_demod, 0, softbits, 0);
	tb->connect(softbits, 0, slicer, 0);
	tb->connect(slicer, 0, start_correlator, 0);
	
	
	tb->connect(start_correlator, 0, deinterleave, 0);

		tb->connect(deinterleave, 0, crc, 0);

	tb->start();



	
	gr_message_sptr msg;




			msg = queue->delete_head();
			parse_message(msg->to_string());	
tb->stop();


  // Exit normally.
  return 0;
}
