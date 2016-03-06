//
//  CRT.cpp
//  Clock Signal
//
//  Created by Thomas Harte on 19/07/2015.
//  Copyright © 2015 Thomas Harte. All rights reserved.
//

#include "CRT.hpp"
#include "CRTOpenGL.hpp"
#include <stdarg.h>
#include <math.h>

using namespace Outputs;

void CRT::set_new_timing(unsigned int cycles_per_line, unsigned int height_of_display, ColourSpace colour_space, unsigned int colour_cycle_numerator, unsigned int colour_cycle_denominator)
{
	_colour_space = colour_space;
	_colour_cycle_numerator = colour_cycle_numerator;
	_colour_cycle_denominator = colour_cycle_denominator;

	const unsigned int syncCapacityLineChargeThreshold = 3;
	const unsigned int millisecondsHorizontalRetraceTime = 7;	// source: Dictionary of Video and Television Technology, p. 234
	const unsigned int scanlinesVerticalRetraceTime = 10;		// source: ibid

																// To quote:
																//
																//	"retrace interval; The interval of time for the return of the blanked scanning beam of
																//	a TV picture tube or camera tube to the starting point of a line or field. It is about 7 µs
																//	for horizontal retrace and 500 to 750 µs for vertical retrace in NTSC and PAL TV."

	_time_multiplier = (2000 + cycles_per_line - 1) / cycles_per_line;

	// store fundamental display configuration properties
	_height_of_display = height_of_display;
	_cycles_per_line = cycles_per_line * _time_multiplier;

	// generate timing values implied by the given arbuments
	_sync_capacitor_charge_threshold = ((syncCapacityLineChargeThreshold * _cycles_per_line) * 50) >> 7;

	// create the two flywheels
	_horizontal_flywheel	= std::unique_ptr<Outputs::Flywheel>(new Outputs::Flywheel(_cycles_per_line, (millisecondsHorizontalRetraceTime * _cycles_per_line) >> 6));
	_vertical_flywheel		= std::unique_ptr<Outputs::Flywheel>(new Outputs::Flywheel(_cycles_per_line * height_of_display, scanlinesVerticalRetraceTime * _cycles_per_line));

	// figure out the divisor necessary to get the horizontal flywheel into a 16-bit range
	unsigned int real_clock_scan_period = (_cycles_per_line * height_of_display) / (_time_multiplier * _common_output_divisor);
	_vertical_flywheel_output_divider = (uint16_t)(ceilf(real_clock_scan_period / 65536.0f) * (_time_multiplier * _common_output_divisor));
}

void CRT::set_new_display_type(unsigned int cycles_per_line, DisplayType displayType)
{
	switch(displayType)
	{
		case DisplayType::PAL50:
			set_new_timing(cycles_per_line, 312, ColourSpace::YUV, 1135, 4);
		break;

		case DisplayType::NTSC60:
			set_new_timing(cycles_per_line, 262, ColourSpace::YIQ, 545, 2);
		break;
	}
}

void CRT::allocate_buffers(unsigned int number, va_list sizes)
{
	_run_builders = new CRTRunBuilder *[kCRTNumberOfFields];
	for(int builder = 0; builder < kCRTNumberOfFields; builder++)
	{
		_run_builders[builder] = new CRTRunBuilder(kCRTOutputVertexSize);
	}
	_composite_src_runs = std::unique_ptr<CRTRunBuilder>(new CRTRunBuilder(kCRTInputVertexSize));

	va_list va;
	va_copy(va, sizes);
	_buffer_builder = std::unique_ptr<CRTInputBufferBuilder>(new CRTInputBufferBuilder(number, va));
	va_end(va);
}

CRT::CRT(unsigned int common_output_divisor) :
	_run_write_pointer(0),
	_sync_capacitor_charge_level(0),
	_is_receiving_sync(false),
	_output_mutex(new std::mutex),
	_visible_area(Rect(0, 0, 1, 1)),
	_sync_period(0),
	_common_output_divisor(common_output_divisor),
	_composite_src_output_y(0),
	_is_writing_composite_run(false)
{
	construct_openGL();
}

CRT::~CRT()
{
	for(int builder = 0; builder < kCRTNumberOfFields; builder++)
	{
		delete _run_builders[builder];
	}
	delete[] _run_builders;
	destruct_openGL();
}

CRT::CRT(unsigned int cycles_per_line, unsigned int common_output_divisor, unsigned int height_of_display, ColourSpace colour_space, unsigned int colour_cycle_numerator, unsigned int colour_cycle_denominator, unsigned int number_of_buffers, ...) : CRT(common_output_divisor)
{
	set_new_timing(cycles_per_line, height_of_display, colour_space, colour_cycle_numerator, colour_cycle_denominator);

	va_list buffer_sizes;
	va_start(buffer_sizes, number_of_buffers);
	allocate_buffers(number_of_buffers, buffer_sizes);
	va_end(buffer_sizes);
}

CRT::CRT(unsigned int cycles_per_line, unsigned int common_output_divisor, DisplayType displayType, unsigned int number_of_buffers, ...) : CRT(common_output_divisor)
{
	set_new_display_type(cycles_per_line, displayType);

	va_list buffer_sizes;
	va_start(buffer_sizes, number_of_buffers);
	allocate_buffers(number_of_buffers, buffer_sizes);
	va_end(buffer_sizes);
}

#pragma mark - Sync loop

Flywheel::SyncEvent CRT::get_next_vertical_sync_event(bool vsync_is_requested, unsigned int cycles_to_run_for, unsigned int *cycles_advanced)
{
	return _vertical_flywheel->get_next_event_in_period(vsync_is_requested, cycles_to_run_for, cycles_advanced);
}

Flywheel::SyncEvent CRT::get_next_horizontal_sync_event(bool hsync_is_requested, unsigned int cycles_to_run_for, unsigned int *cycles_advanced)
{
	return _horizontal_flywheel->get_next_event_in_period(hsync_is_requested, cycles_to_run_for, cycles_advanced);
}

#define output_position_x(v)		(*(uint16_t *)&next_run[kCRTOutputVertexSize*v + kCRTOutputVertexOffsetOfPosition + 0])
#define output_position_y(v)		(*(uint16_t *)&next_run[kCRTOutputVertexSize*v + kCRTOutputVertexOffsetOfPosition + 2])
#define output_tex_x(v)				(*(uint16_t *)&next_run[kCRTOutputVertexSize*v + kCRTOutputVertexOffsetOfTexCoord + 0])
#define output_tex_y(v)				(*(uint16_t *)&next_run[kCRTOutputVertexSize*v + kCRTOutputVertexOffsetOfTexCoord + 2])
#define output_lateral(v)			next_run[kCRTOutputVertexSize*v + kCRTOutputVertexOffsetOfLateral]
#define output_timestamp(v)			(*(uint32_t *)&next_run[kCRTOutputVertexSize*v + kCRTOutputVertexOffsetOfTimestamp])

#define input_input_position_x(v)	(*(uint16_t *)&next_run[kCRTInputVertexSize*v + kCRTInputVertexOffsetOfInputPosition + 0])
#define input_input_position_y(v)	(*(uint16_t *)&next_run[kCRTInputVertexSize*v + kCRTInputVertexOffsetOfInputPosition + 2])
#define input_output_position_x(v)	(*(uint16_t *)&next_run[kCRTInputVertexSize*v + kCRTInputVertexOffsetOfOutputPosition + 0])
#define input_output_position_y(v)	(*(uint16_t *)&next_run[kCRTInputVertexSize*v + kCRTInputVertexOffsetOfOutputPosition + 2])
#define input_phase(v)				next_run[kCRTOutputVertexSize*v + kCRTInputVertexOffsetOfPhaseAndAmplitude + 0]
#define input_amplitude(v)			next_run[kCRTOutputVertexSize*v + kCRTInputVertexOffsetOfPhaseAndAmplitude + 1]
#define input_phase_time(v)			(*(uint16_t *)&next_run[kCRTOutputVertexSize*v + kCRTInputVertexOffsetOfPhaseTime])

void CRT::advance_cycles(unsigned int number_of_cycles, unsigned int source_divider, bool hsync_requested, bool vsync_requested, const bool vsync_charging, const Type type, uint16_t tex_x, uint16_t tex_y)
{
	number_of_cycles *= _time_multiplier;

	bool is_output_run = ((type == Type::Level) || (type == Type::Data));

	while(number_of_cycles) {

		unsigned int time_until_vertical_sync_event, time_until_horizontal_sync_event;
		Flywheel::SyncEvent next_vertical_sync_event = get_next_vertical_sync_event(vsync_requested, number_of_cycles, &time_until_vertical_sync_event);
		Flywheel::SyncEvent next_horizontal_sync_event = get_next_horizontal_sync_event(hsync_requested, time_until_vertical_sync_event, &time_until_horizontal_sync_event);

		// get the next sync event and its timing; hsync request is instantaneous (being edge triggered) so
		// set it to false for the next run through this loop (if any)
		unsigned int next_run_length = std::min(time_until_vertical_sync_event, time_until_horizontal_sync_event);

		hsync_requested = false;
		vsync_requested = false;

		bool is_output_segment = ((is_output_run && next_run_length) && !_horizontal_flywheel->is_in_retrace() && !_vertical_flywheel->is_in_retrace());
		uint8_t *next_run = nullptr;
		if(is_output_segment)
		{
			_output_mutex->lock();
			next_run = (_output_device == CRT::Monitor) ? _run_builders[_run_write_pointer]->get_next_run(6) : _composite_src_runs->get_next_run(2);
		}

		//	Vertex output is arranged for triangle strips, as:
		//
		//	2			[4/5]
		//
		//	[0/1]		3
		if(next_run)
		{
			if(_output_device == CRT::Monitor)
			{
				// set the type, initial raster position and type of this run
				output_position_x(0) = output_position_x(1) = output_position_x(2) = (uint16_t)_horizontal_flywheel->get_current_output_position();
				output_position_y(0) = output_position_y(1) = output_position_y(2) = (uint16_t)(_vertical_flywheel->get_current_output_position() / _vertical_flywheel_output_divider);
				output_timestamp(0) = output_timestamp(1) = output_timestamp(2) = _run_builders[_run_write_pointer]->duration;
				output_tex_x(0) = output_tex_x(1) = output_tex_x(2) = tex_x;

				// these things are constants across the line so just throw them out now
				output_tex_y(0) = output_tex_y(1) = output_tex_y(2) = output_tex_y(3) = output_tex_y(4) = output_tex_y(5) = tex_y;
				output_lateral(0) = output_lateral(1) = output_lateral(3) = 0;
				output_lateral(2) = output_lateral(4) = output_lateral(5) = 1;
			}
			else
			{
				input_input_position_x(0) = tex_x;
				input_input_position_y(0) = input_input_position_y(1) = tex_y;
				input_output_position_x(0) = (uint16_t)_horizontal_flywheel->get_current_output_position();
				input_output_position_y(0) = input_output_position_y(1) = _composite_src_output_y;
				input_phase(0) = input_phase(1) = _colour_burst_phase;
				input_amplitude(0) = input_amplitude(1) = _colour_burst_amplitude;
				input_phase_time(0) = input_phase_time(1) = _colour_burst_time;
			}
		}

		// decrement the number of cycles left to run for and increment the
		// horizontal counter appropriately
		number_of_cycles -= next_run_length;
		_run_builders[_run_write_pointer]->duration += next_run_length;

		// either charge or deplete the vertical retrace capacitor (making sure it stops at 0)
		if (vsync_charging && !_vertical_flywheel->is_in_retrace())
			_sync_capacitor_charge_level += next_run_length;
		else
			_sync_capacitor_charge_level = std::max(_sync_capacitor_charge_level - (int)next_run_length, 0);

		// react to the incoming event...
		_horizontal_flywheel->apply_event(next_run_length, (next_run_length == time_until_horizontal_sync_event) ? next_horizontal_sync_event : Flywheel::SyncEvent::None);
		_vertical_flywheel->apply_event(next_run_length, (next_run_length == time_until_vertical_sync_event) ? next_vertical_sync_event : Flywheel::SyncEvent::None);

		if(next_run)
		{
			// if this is a data run then advance the buffer pointer
			if(type == Type::Data && source_divider) tex_x += next_run_length / (_time_multiplier * source_divider);

			if(_output_device == CRT::Monitor)
			{
				// store the final raster position
				output_position_x(3) = output_position_x(4) = output_position_x(5) = (uint16_t)_horizontal_flywheel->get_current_output_position();
				output_position_y(3) = output_position_y(4) = output_position_y(5) = (uint16_t)(_vertical_flywheel->get_current_output_position() / _vertical_flywheel_output_divider);
				output_timestamp(3) = output_timestamp(4) = output_timestamp(5) = _run_builders[_run_write_pointer]->duration;
				output_tex_x(3) = output_tex_x(4) = output_tex_x(5) = tex_x;
			}
			else
			{
				input_input_position_x(1) = tex_x;
				input_output_position_x(1) = (uint16_t)_horizontal_flywheel->get_current_output_position();
			}
		}

		if(is_output_segment)
		{
			_output_mutex->unlock();
		}

		// if this is horizontal retrace then advance the output line counter and bookend an output run
		if(_output_device == CRT::Television)
		{
			Flywheel::SyncEvent honoured_event = (next_run_length == time_until_vertical_sync_event) ? next_vertical_sync_event : next_horizontal_sync_event;
			bool needs_endpoint =
				(honoured_event == Flywheel::SyncEvent::StartRetrace && _is_writing_composite_run) ||
				(honoured_event == Flywheel::SyncEvent::EndRetrace && !_horizontal_flywheel->is_in_retrace() && !_vertical_flywheel->is_in_retrace());

			if(needs_endpoint)
			{
				uint8_t *next_run = _run_builders[_run_write_pointer]->get_next_run(3);

				output_position_x(0) = output_position_x(1) = output_position_x(2) = (uint16_t)_horizontal_flywheel->get_current_output_position();
				output_position_y(0) = output_position_y(1) = output_position_y(2) = (uint16_t)(_vertical_flywheel->get_current_output_position() / _vertical_flywheel_output_divider);
				output_timestamp(0) = output_timestamp(1) = output_timestamp(2) = _run_builders[_run_write_pointer]->duration;
				output_tex_x(0) = output_tex_x(1) = output_tex_x(2) = tex_x;
				output_tex_y(0) = output_tex_y(1) = output_tex_y(2) = tex_y;
				output_lateral(0) = 0;
				output_lateral(1) = _is_writing_composite_run ? 1 : 0;
				output_lateral(2) = 1;

				_is_writing_composite_run ^= true;
			}

			if(next_run_length == time_until_horizontal_sync_event && next_horizontal_sync_event == Flywheel::SyncEvent::EndRetrace)
			{
				_composite_src_output_y = (_composite_src_output_y + 1) % CRTIntermediateBufferHeight;
			}
		}

		// if this is vertical retrace then adcance a field
		if(next_run_length == time_until_vertical_sync_event && next_vertical_sync_event == Flywheel::SyncEvent::EndRetrace)
		{
			// TODO: how to communicate did_detect_vsync? Bring the delegate back?
//			_delegate->crt_did_end_frame(this, &_current_frame_builder->frame, _did_detect_vsync);

			_run_write_pointer = (_run_write_pointer + 1)%kCRTNumberOfFields;
			_run_builders[_run_write_pointer]->reset();
		}
	}
}

#undef output_position_x
#undef output_position_y
#undef output_tex_x
#undef output_tex_y
#undef output_lateral
#undef output_timestamp

#undef input_input_position_x
#undef input_input_position_y
#undef input_output_position_x
#undef input_output_position_y
#undef input_phase
#undef input_amplitude
#undef input_phase_age

#pragma mark - stream feeding methods

void CRT::output_scan(Scan *scan)
{
	bool this_is_sync = (scan->type == Type::Sync);
	bool is_trailing_edge = (_is_receiving_sync && !this_is_sync);
	bool hsync_requested = is_trailing_edge && (_sync_period < (_horizontal_flywheel->get_scan_period() >> 2));
	bool vsync_requested = is_trailing_edge && (_sync_capacitor_charge_level >= _sync_capacitor_charge_threshold);
	_is_receiving_sync = this_is_sync;

	// simplified colour burst logic: if it's within the back porch we'll take it
	if(scan->type == Type::ColourBurst)
	{
		if(_horizontal_flywheel->get_current_time() < (_horizontal_flywheel->get_standard_period() * 12) >> 6)
		{
			_colour_burst_time = (uint16_t)_colour_burst_time;
			_colour_burst_phase = scan->phase;
			_colour_burst_amplitude = scan->amplitude;
		}
	}

	// TODO: inspect raw data for potential colour burst if required

	_sync_period = _is_receiving_sync ? (_sync_period + scan->number_of_cycles) : 0;
	advance_cycles(scan->number_of_cycles, scan->source_divider, hsync_requested, vsync_requested, this_is_sync, scan->type, scan->tex_x, scan->tex_y);
}

/*
	These all merely channel into advance_cycles, supplying appropriate arguments
*/
void CRT::output_sync(unsigned int number_of_cycles)
{
	Scan scan{
		.type = Type::Sync,
		.number_of_cycles = number_of_cycles
	};
	output_scan(&scan);
}

void CRT::output_blank(unsigned int number_of_cycles)
{
	Scan scan {
		.type = Type::Blank,
		.number_of_cycles = number_of_cycles
	};
	output_scan(&scan);
}

void CRT::output_level(unsigned int number_of_cycles)
{
	Scan scan {
		.type = Type::Level,
		.number_of_cycles = number_of_cycles,
		.tex_x = _buffer_builder->_write_x_position,
		.tex_y = _buffer_builder->_write_y_position
	};
	output_scan(&scan);
}

void CRT::output_colour_burst(unsigned int number_of_cycles, uint8_t phase, uint8_t amplitude)
{
	Scan scan {
		.type = Type::ColourBurst,
		.number_of_cycles = number_of_cycles,
		.phase = phase,
		.amplitude = amplitude
	};
	output_scan(&scan);
}

void CRT::output_data(unsigned int number_of_cycles, unsigned int source_divider)
{
	_buffer_builder->reduce_previous_allocation_to(number_of_cycles / source_divider);
	Scan scan {
		.type = Type::Data,
		.number_of_cycles = number_of_cycles,
		.tex_x = _buffer_builder->_write_x_position,
		.tex_y = _buffer_builder->_write_y_position,
		.source_divider = source_divider
	};
	output_scan(&scan);
}

#pragma mark - Buffer supply

void CRT::allocate_write_area(size_t required_length)
{
	_output_mutex->lock();
	_buffer_builder->allocate_write_area(required_length);
	_output_mutex->unlock();
}

uint8_t *CRT::get_write_target_for_buffer(int buffer)
{
	return _buffer_builder->get_write_target_for_buffer(buffer);
}
