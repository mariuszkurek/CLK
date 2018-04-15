//
//  AppleII.cpp
//  Clock Signal
//
//  Created by Thomas Harte on 14/04/2018.
//  Copyright © 2018 Thomas Harte. All rights reserved.
//

#include "AppleII.hpp"

#include "../CRTMachine.hpp"
#include "../Utility/MemoryFuzzer.hpp"

#include "../../Processors/6502/6502.hpp"

#include "Video.hpp"

#include <memory>

namespace {

class ConcreteMachine:
	public CRTMachine::Machine,
	public CPU::MOS6502::BusHandler,
	public AppleII::Machine {
	public:

		ConcreteMachine():
		 	m6502_(*this) {
			set_clock_rate(1022727);
			Memory::Fuzz(ram_, sizeof(ram_));
		}

		void setup_output(float aspect_ratio) override {
			video_.reset(new AppleII::Video);
		}

		void close_output() override {
			video_.reset();
		}

		Outputs::CRT::CRT *get_crt() override {
			return video_->get_crt();
		}

		Outputs::Speaker::Speaker *get_speaker() override {
			return nullptr;
		}

		Cycles perform_bus_operation(CPU::MOS6502::BusOperation operation, uint16_t address, uint8_t *value) {
			++ cycles_since_video_update_;

			switch(address) {
				default:
					if(isReadOperation(operation)) {
						if(address < sizeof(ram_)) {
							*value = ram_[address];
						} else if(address >= rom_start_address_) {
							*value = rom_[address - rom_start_address_];
						} else {
							switch(address) {
								default:	*value = 0xff;	break;
								case 0xc000:
									// TODO: read keyboard.
									*value = 0;
								break;
							}
						}
					} else {
						if(address < sizeof(ram_)) {
							update_video();	// TODO: be more selective.
							ram_[address] = *value;
						}
					}
				break;

				case 0xc050:	update_video();		video_->set_graphics_mode();	break;
				case 0xc051:	update_video();		video_->set_text_mode();		break;
				case 0xc052:	update_video();		video_->set_mixed_mode(false);	break;
				case 0xc053:	update_video();		video_->set_mixed_mode(true);	break;
				case 0xc054:	update_video();		video_->set_video_page(0);		break;
				case 0xc055:	update_video();		video_->set_video_page(1);		break;
				case 0xc056:	update_video();		video_->set_low_resolution();	break;
				case 0xc057:	update_video();		video_->set_high_resolution();	break;
			}

			// The Apple II has a slightly weird timing pattern: every 65th CPU cycle is stretched
			// by an extra 1/7th. That's because one cycle lasts 3.5 NTSC colour clocks, so after
			// 65 cycles a full line of 227.5 colour clocks have passed. But the high-rate binary
			// signal approximation that produces colour needs to be in phase, so a stretch of exactly
			// 0.5 further colour cycles is added.
			cycles_into_current_line_ = (cycles_into_current_line_ + 1) % 65;
			if(!cycles_into_current_line_) {
				// Do something. Do something else.
			}

			return Cycles(1);
		}

		void flush() {
			update_video();
		}

		bool set_rom_fetcher(const std::function<std::vector<std::unique_ptr<std::vector<uint8_t>>>(const std::string &machine, const std::vector<std::string> &names)> &roms_with_names) override {
			auto roms = roms_with_names(
				"AppleII",
				{
					"apple2o.rom"
				});

			if(!roms[0]) return false;
			rom_ = std::move(*roms[0]);
			rom_start_address_ = static_cast<uint16_t>(0x10000 - rom_.size());

			return true;
		}

		void run_for(const Cycles cycles) override {
			m6502_.run_for(cycles);
		}

	private:
		CPU::MOS6502::Processor<ConcreteMachine, false> m6502_;
		std::unique_ptr<AppleII::Video> video_;
		int cycles_into_current_line_ = 0;
		Cycles cycles_since_video_update_;

		void update_video() {
			video_->run_for(cycles_since_video_update_.flush());
		}

		uint8_t ram_[48*1024];
		std::vector<uint8_t> rom_;
		uint16_t rom_start_address_;
};

}

using namespace AppleII;

Machine *Machine::AppleII() {
	return new ConcreteMachine;
}

Machine::~Machine() {}
