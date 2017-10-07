//
//  CPCDSK.cpp
//  Clock Signal
//
//  Created by Thomas Harte on 05/08/2017.
//  Copyright © 2017 Thomas Harte. All rights reserved.
//

#include "CPCDSK.hpp"

#include "../../Encodings/MFM/Encoder.hpp"

using namespace Storage::Disk;

CPCDSK::CPCDSK(const char *file_name) :
	Storage::FileHolder(file_name), is_extended_(false) {
	if(!check_signature("MV - CPC", 8)) {
		is_extended_ = true;
		fseek(file_, 0, SEEK_SET);
		if(!check_signature("EXTENDED", 8))
			throw ErrorNotCPCDSK;
	}

	// Don't really care about about the creator; skip.
	fseek(file_, 0x30, SEEK_SET);
	head_position_count_ = fgetc(file_);
	head_count_ = fgetc(file_);

	if(is_extended_) {
		// Skip two unused bytes and grab the track size table.
		fseek(file_, 2, SEEK_CUR);
		for(int c = 0; c < head_position_count_ * head_count_; c++) {
			track_sizes_.push_back((size_t)(fgetc(file_) << 8));
		}
	} else {
		size_of_a_track_ = fgetc16le();
	}
}

int CPCDSK::get_head_position_count() {
	return head_position_count_;
}

int CPCDSK::get_head_count() {
	return head_count_;
}

std::shared_ptr<Track> CPCDSK::get_track_at_position(Track::Address address) {
	// Given that thesea are interleaved images, determine which track, chronologically, is being requested.
	size_t chronological_track = static_cast<size_t>((address.position * head_count_) + address.head);

	// All DSK images reserve 0x100 bytes for their headers.
	long file_offset = 0x100;
	if(is_extended_) {
		// Tracks are a variable size in the original DSK file format.

		// Check that there is anything stored for this track.
		if(!track_sizes_[chronological_track]) {
			return nullptr;
		}

		// Sum the lengths of all tracks prior to the interesting one to get a file offset.
		size_t t = 0;
		while(t < chronological_track && t < track_sizes_.size()) {
			file_offset += track_sizes_[t];
			t++;
		}
	} else {
		// Tracks are a fixed size in the original DSK file format.
		file_offset += size_of_a_track_ * static_cast<long>(chronological_track);
	}

	// Find the track, and skip the unused part of track information.
	fseek(file_, file_offset + 16, SEEK_SET);

	// Grab the track information.
	fseek(file_, 5, SEEK_CUR);	// skip track number, side number, sector size — each is given per sector
	int number_of_sectors = fgetc(file_);
	uint8_t gap3_length = (uint8_t)fgetc(file_);
	uint8_t filler_byte = (uint8_t)fgetc(file_);

	// Grab the sector information
	struct SectorInfo {
		uint8_t track;
		uint8_t side;
		uint8_t sector;
		uint8_t length;
		uint8_t status1;
		uint8_t status2;
		size_t actual_length;
	};
	std::vector<SectorInfo> sector_infos;
	while(number_of_sectors--) {
		SectorInfo sector_info;

		sector_info.track = (uint8_t)fgetc(file_);
		sector_info.side = (uint8_t)fgetc(file_);
		sector_info.sector = (uint8_t)fgetc(file_);
		sector_info.length = (uint8_t)fgetc(file_);
		sector_info.status1 = (uint8_t)fgetc(file_);
		sector_info.status2 = (uint8_t)fgetc(file_);
		sector_info.actual_length = fgetc16le();

		sector_infos.push_back(sector_info);
	}

	// Get the sectors.
	fseek(file_, file_offset + 0x100, SEEK_SET);
	std::vector<Storage::Encodings::MFM::Sector> sectors;
	for(auto &sector_info : sector_infos) {
		Storage::Encodings::MFM::Sector new_sector;
		new_sector.address.track = sector_info.track;
		new_sector.address.side = sector_info.side;
		new_sector.address.sector = sector_info.sector;
		new_sector.size = sector_info.length;

		size_t data_size;
		if(is_extended_) {
			data_size = sector_info.actual_length;
		} else {
			data_size = (size_t)(128 << sector_info.length);
			if(data_size == 0x2000) data_size = 0x1800;
		}
		new_sector.data.resize(data_size);
		fread(new_sector.data.data(), sizeof(uint8_t), data_size, file_);

		if(sector_info.status2 & 0x20) {
			// The CRC failed in the data field.
			new_sector.has_data_crc_error = true;
		} else {
			if(sector_info.status1 & 0x20) {
				// The CRC failed in the ID field.
				new_sector.has_header_crc_error = true;
			}
		}

		if(sector_info.status2 & 0x40) {
			// This sector is marked as deleted.
			new_sector.is_deleted = true;
		}

		if(sector_info.status2 & 0x01) {
			// Data field wasn't found.
			new_sector.data.clear();
		}

		sectors.push_back(std::move(new_sector));
	}

	// TODO: extensions to the extended format; John Elliot's addition of single-density support,
	// and Simon Owen's weak/random sectors, subject to adding some logic to pick a potential
	// FM/MFM encoding that can produce specified weak values.

	if(sectors.size()) return Storage::Encodings::MFM::GetMFMTrackWithSectors(sectors, gap3_length, filler_byte);

	return nullptr;
}
