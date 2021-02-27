/* Copyright 2021 Hein-Pieter van Braam-Stewart
 *
 * This file is part of ppelib (Portable Portable Executable LIBrary)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <ppelib/ppelib-constants.h>
#include <ppelib-error.h>
#include <ppelib-generated.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "export.h"
#include "main.h"

EXPORT_SYM ppelib_file_t* ppelib_create() {
	ppelib_reset_error();

	ppelib_file_t *pe = calloc(sizeof(ppelib_file_t), 1);
	if (!pe) {
		ppelib_set_error("Failed to allocate PE structure");
	}

	return pe;
}

EXPORT_SYM void ppelib_destroy(ppelib_file_t *pe) {
	if (!pe) {
		return;
	}

	free(pe->stub);
	for (size_t i = 0; i < pe->header.number_of_sections; ++i) {
		free(pe->sections[i]->contents);
		free(pe->sections[i]);
	}
	for (size_t i = 0; i < pe->certificate_table.size; ++i) {
		free(pe->certificate_table.certificates[i].certificate);
	}
	free(pe->certificate_table.certificates);
	free(pe->data_directories);
	free(pe->header.data_directories);
	free(pe->sections);
	free(pe->trailing_data);
	free(pe->file_contents);

	free(pe);
}

EXPORT_SYM ppelib_file_t* ppelib_create_from_buffer(uint8_t *buffer, size_t size) {
	ppelib_reset_error();

	if (size < PE_SIGNATURE_OFFSET + sizeof(uint32_t)) {
		ppelib_set_error("Not a PE file (file too small)");
		free(buffer);
		return NULL;
	}

	uint32_t header_offset = read_uint32_t(buffer + PE_SIGNATURE_OFFSET);
	if (size < header_offset + sizeof(uint32_t)) {
		ppelib_set_error("Not a PE file (file too small for PE signature)");
		free(buffer);
		return NULL;
	}

	uint32_t signature = read_uint32_t(buffer + header_offset);
	if (signature != PE_SIGNATURE) {
		ppelib_set_error("Not a PE file (PE00 signature missing)");
		free(buffer);
		return NULL;
	}

	ppelib_file_t *pe = ppelib_create();
	if (ppelib_error_peek()) {
		ppelib_destroy(pe);
		return NULL;
	}

	pe->file_size = size;
	pe->file_contents = buffer;
	pe->pe_header_offset = header_offset;
	pe->coff_header_offset = header_offset + 4;

	if (size < pe->coff_header_offset + COFF_HEADER_SIZE) {
		ppelib_set_error("Not a PE file (file too small for COFF header)");
		ppelib_destroy(pe);
		return NULL;
	}

	size_t header_size = deserialize_pe_header(pe->file_contents, pe->coff_header_offset, pe->file_size, &pe->header);
	if (ppelib_error_peek()) {
		ppelib_destroy(pe);
		return NULL;
	}

	pe->section_offset = header_size + pe->coff_header_offset;
	pe->sections = malloc(sizeof(ppelib_section_t*) * pe->header.number_of_sections);
	if (!pe->sections) {
		ppelib_set_error("Failed to allocate sections");
		ppelib_destroy(pe);
		return NULL;
	}

	pe->data_directories = calloc(sizeof(data_directory_t) * pe->header.number_of_rva_and_sizes, 1);
	if (!pe->data_directories) {
		ppelib_set_error("Failed to allocate data directories");
		ppelib_destroy(pe);
		return NULL;
	}

	pe->end_of_sections = 0;

	for (uint32_t i = 0; i < pe->header.number_of_sections; ++i) {
		pe->sections[i] = malloc(sizeof(ppelib_section_t));
		if (!pe->sections[i]) {
			ppelib_set_error("Failed to allocate section");
			ppelib_destroy(pe);
			return NULL;
		}

		size_t section_size = deserialize_section(pe->file_contents, pe->section_offset + (i * PE_SECTION_HEADER_SIZE),
				pe->file_size, pe->sections[i]);

		if (ppelib_error_peek()) {
			ppelib_destroy(pe);
			return NULL;
		}

		if (section_size > pe->end_of_sections) {
			pe->end_of_sections = section_size;
		}

		for (uint32_t d = 0; d < pe->header.number_of_rva_and_sizes; ++d) {
			size_t directory_va = pe->header.data_directories[d].virtual_address;
			size_t directory_size = pe->header.data_directories[d].size;
			size_t section_va = pe->sections[i]->virtual_address;
			size_t section_end = section_va + pe->sections[i]->size_of_raw_data;

			if (section_va <= directory_va) {
				//printf("Considering directory %s in section %s\n", data_directory_names[d], pe.sections[i]->name);
				if (section_end >= directory_va) {
					pe->data_directories[d].section = pe->sections[i];
					pe->data_directories[d].offset = directory_va - section_va;
					pe->data_directories[d].size = directory_size;
					pe->data_directories[d].orig_rva = directory_va;
					pe->data_directories[d].orig_size = directory_size;

					//printf("Found directory %s in section %s\n", data_directory_names[d], pe.sections[i]->name);
				} else {
					//printf("Directory %s doesn't fit in section %s. 0x%08lx < 0x%08lx\n", data_directory_names[d], pe.sections[i]->name, section_end, directory_va);
				}
			}
		}
	}

	if (pe->header.data_directories[DIR_CERTIFICATE_TABLE].size) {
		deserialize_certificate_table(pe->file_contents, &pe->header, pe->file_size, &pe->certificate_table);
		if (ppelib_error_peek()) {
			ppelib_destroy(pe);
			return NULL;
		}
	}

	pe->start_of_sections = pe->sections[0]->virtual_address;

	//void* t = pe.sections[4];
	//pe.sections[4] = pe.sections[3];
	//pe.sections[3] = t;

	pe->stub = malloc(pe->pe_header_offset);
	if (!pe->stub) {
		ppelib_set_error("Failed to allocate memory for PE stub");
		ppelib_destroy(pe);
		return NULL;
	}
	memcpy(pe->stub, pe->file_contents, pe->pe_header_offset);

	if (size > pe->end_of_sections) {
		pe->trailing_data_size = size - pe->end_of_sections;
		pe->trailing_data = malloc(pe->trailing_data_size);

		if (!pe->trailing_data) {
			ppelib_set_error("Failed to allocate memory for trailing data");
			ppelib_destroy(pe);
			return NULL;
		}

		memcpy(pe->trailing_data, pe->file_contents + pe->end_of_sections, pe->trailing_data_size);
	}

	return pe;
}

EXPORT_SYM ppelib_file_t* ppelib_create_from_file(const char *filename) {
	ppelib_reset_error();
	size_t file_size;
	uint8_t *file_contents;

	FILE *f = fopen(filename, "rb");

	if (!f) {
		ppelib_set_error("Failed to open file");
		return NULL;
	}

	fseek(f, 0, SEEK_END);
	file_size = ftell(f);
	rewind(f);

	file_contents = malloc(file_size);
	if (!file_size) {
		fclose(f);
		ppelib_set_error("Failed to allocate file data");
		return NULL;
	}

	size_t retsize = fread(file_contents, 1, file_size, f);
	if (retsize != file_size) {
		fclose(f);
		ppelib_set_error("Failed to read file data");
		return NULL;
	}

	fclose(f);

	return ppelib_create_from_buffer(file_contents, file_size);
}

EXPORT_SYM size_t ppelib_write_to_buffer(ppelib_file_t *pe, uint8_t *buffer, size_t buf_size) {
	ppelib_reset_error();

	size_t size = 0;

	// Write stub
	size += pe->pe_header_offset;
	size += 4;
	size_t coff_header_size = serialize_pe_header(&pe->header, NULL, pe->pe_header_offset);
	if (ppelib_error_peek()) {
		return 0;
	}

	size += coff_header_size;
	size_t end_of_sections = 0;

	size_t section_offset = pe->pe_header_offset + coff_header_size;
	for (uint32_t i = 0; i < pe->header.number_of_sections; ++i) {
		size_t section_size = serialize_section(pe->sections[i], NULL, section_offset + (i * PE_SECTION_HEADER_SIZE));
		if (ppelib_error_peek()) {
			return 0;
		}

		if (section_size > end_of_sections) {
			end_of_sections = section_size;
		}
	}

	// Theoretically all the sections could be before the header
	if (end_of_sections > size) {
		size = end_of_sections;
	}

	size += pe->trailing_data_size;

	size_t certificates_size = 0;
	if (pe->header.data_directories[DIR_CERTIFICATE_TABLE].size) {
		certificates_size = serialize_certificate_table(&pe->certificate_table, NULL);
		if (ppelib_error_peek()) {
			return 0;
		}

		if (certificates_size > size) {
			size = certificates_size;
		}
	}

	if (buffer && size > buf_size) {
		ppelib_set_error("Target buffer too small.");
		return 0;
	}

	if (!buffer) {
		return size;
	}

	size_t write = 0;

	memset(buffer, 0, size);
	//memset(buffer, 0xCC, size);

	memcpy(buffer, pe->stub, pe->pe_header_offset);
	write += pe->pe_header_offset;

	// Write PE header
	memcpy(buffer + write, "PE\0", 4);
	write += 4;

	// Write COFF header
	serialize_pe_header(&pe->header, buffer, pe->pe_header_offset + 4);
	if (ppelib_error_peek()) {
		return 0;
	}

	// Write sections
	for (uint32_t i = 0; i < pe->header.number_of_sections; ++i) {
		serialize_section(pe->sections[i], buffer, section_offset + 4 + (i * PE_SECTION_HEADER_SIZE));
		if (ppelib_error_peek()) {
			return 0;
		}
	}

	// Write trailing data
	memcpy(buffer + end_of_sections, pe->trailing_data, pe->trailing_data_size);

	// Write certificates
	serialize_certificate_table(&pe->certificate_table, buffer);
	if (ppelib_error_peek()) {
		return 0;
	}

	return size;
}

EXPORT_SYM size_t ppelib_write_to_file(ppelib_file_t *pe, const char *filename) {
	ppelib_reset_error();

	FILE* f = fopen(filename, "wb");
	if (!f) {
		ppelib_set_error("Failed to open file");
		return 0;
	}

	size_t bufsize = ppelib_write_to_buffer(pe, NULL, 0);
	if (ppelib_error_peek()) {
		fclose(f);
		return 0;
	}

	uint8_t* buffer = malloc(bufsize);
	if (!buffer) {
		ppelib_set_error("Failed to allocate buffer");
		fclose(f);
		return 0;
	}

	ppelib_write_to_buffer(pe, buffer, bufsize);
	if (ppelib_error_peek()) {
		fclose(f);
		free(buffer);
		return 0;
	}

	size_t written = fwrite(buffer, 1, bufsize, f);
	fclose(f);
	free(buffer);

	if (written != bufsize) {
		ppelib_set_error("Failed to write data");
	}

	return written;
}

EXPORT_SYM ppelib_header_t* ppelib_get_header(ppelib_file_t *pe) {
	ppelib_reset_error();

	ppelib_header_t *retval = malloc(sizeof(ppelib_header_t));
	if (!retval) {
		ppelib_set_error("Unable to allocate header");
		return NULL;
	}

	memcpy(retval, &pe->header, sizeof(ppelib_header_t));
	return retval;
}

EXPORT_SYM void ppelib_set_header(ppelib_file_t *pe, ppelib_header_t *header) {
	ppelib_reset_error();

	if (header->magic != PE32_MAGIC && header->magic != PE32PLUS_MAGIC) {
		ppelib_set_error("Unknown magic");
		return;
	}

	if (header->number_of_sections != pe->header.number_of_sections) {
		ppelib_set_error("number_of_sections mismatch");
		return;
	}

	if (header->number_of_rva_and_sizes != pe->header.number_of_rva_and_sizes) {
		ppelib_set_error("number_of_rva_and_sizes mismatch");
	}

	if (header->size_of_headers != pe->header.size_of_headers) {
		ppelib_set_error("size_of_headers mismatch");
	}

	memcpy(&pe->header, header, sizeof(ppelib_header_t));
}

EXPORT_SYM void ppelib_free_header(ppelib_header_t *header) {
	free(header);
}

int write_pe_file(const char *filename, const ppelib_file_t *pe) {
	uint8_t *buffer = NULL;
	size_t size = 0;
	size_t write = 0;

	// Write stub
	size += pe->pe_header_offset;
	size += 4;
	size_t coff_header_size = serialize_pe_header(&pe->header, NULL, pe->pe_header_offset);
	size += coff_header_size;
	size_t end_of_sections = 0;

	size_t section_offset = pe->pe_header_offset + coff_header_size;
	for (uint32_t i = 0; i < pe->header.number_of_sections; ++i) {
		size_t section_size = serialize_section(pe->sections[i], NULL, section_offset + (i * PE_SECTION_HEADER_SIZE));

		if (section_size > end_of_sections) {
			end_of_sections = section_size;
		}
	}

	// Theoretically all the sections could be before the header
	if (end_of_sections > size) {
		size = end_of_sections;
	}

	size += pe->trailing_data_size;

	size_t certificates_size = serialize_certificate_table(&pe->certificate_table, NULL);

	if (certificates_size > size) {
		size = certificates_size;
	}

	printf("Size of coff_header        : %li\n", coff_header_size);
	printf("Size of sections           : %li\n", end_of_sections);
	printf("Size of trailing data      : %li\n", pe->trailing_data_size);
	printf("Total size                 : %li\n", size);

	buffer = realloc(buffer, size);
	if (!buffer) {
		fprintf(stderr, "Failed to allocate\n");
		return 1;
	}
	memset(buffer, 0, size);
	//memset(buffer, 0xCC, size);

	memcpy(buffer, pe->stub, pe->pe_header_offset);
	write += pe->pe_header_offset;

	// Write PE header
	memcpy(buffer + write, "PE\0", 4);
	write += 4;

	// Write COFF header
	serialize_pe_header(&pe->header, buffer, pe->pe_header_offset + 4);

	// Write sections
	for (uint32_t i = 0; i < pe->header.number_of_sections; ++i) {
		serialize_section(pe->sections[i], buffer, section_offset + 4 + (i * PE_SECTION_HEADER_SIZE));
	}

	// Write trailing data
	memcpy(buffer + end_of_sections, pe->trailing_data, pe->trailing_data_size);

	// Write certificates
	serialize_certificate_table(&pe->certificate_table, buffer);

	FILE *f = fopen(filename, "w+");
	fwrite(buffer, 1, size, f);

	fclose(f);
	free(buffer);

	return size;
}

EXPORT_SYM void ppelib_recalculate(ppelib_file_t *pe) {
	size_t coff_header_size = serialize_pe_header(&pe->header, NULL, pe->pe_header_offset);
	size_t size_of_headers = pe->pe_header_offset + 4 + coff_header_size
			+ (pe->header.number_of_sections * PE_SECTION_HEADER_SIZE);

	size_t next_section_virtual = pe->start_of_sections;
	size_t next_section_physical = pe->header.size_of_headers;

	uint32_t base_of_code = 0;
	uint32_t base_of_data = 0;
	uint32_t size_of_initialized_data = 0;
	uint32_t size_of_uninitialized_data = 0;
	uint32_t size_of_code = 0;

	for (uint32_t i = 0; i < pe->header.number_of_sections; ++i) {
		ppelib_section_t *section = pe->sections[i];

		if (section->size_of_raw_data && section->virtual_size <= section->size_of_raw_data) {
			section->size_of_raw_data = TO_NEAREST(section->virtual_size, pe->header.file_alignment);
		}

		section->virtual_address = next_section_virtual;

		if (section->size_of_raw_data) {
			section->pointer_to_raw_data = next_section_physical;
		}

		next_section_virtual =
		TO_NEAREST(section->virtual_size, pe->header.section_alignment) + next_section_virtual;
		next_section_physical =
		TO_NEAREST(section->size_of_raw_data, pe->header.file_alignment) + next_section_physical;

		if (CHECK_BIT(section->characteristics, IMAGE_SCN_CNT_CODE)) {
			if (!base_of_code) {
				base_of_code = section->virtual_address;
			}

			// This appears to hold empirically true.
			if (strcmp(".bind", section->name) != 0) {
				size_of_code += TO_NEAREST(section->virtual_size, pe->header.file_alignment);
			}
		}

		if (!base_of_data && !CHECK_BIT(section->characteristics, IMAGE_SCN_CNT_CODE)) {
			base_of_data = section->virtual_address;
		}

		if (CHECK_BIT(section->characteristics, IMAGE_SCN_CNT_INITIALIZED_DATA)) {
			// This appears to hold empirically true.
			if (pe->header.magic == PE32_MAGIC) {
				uint32_t vs = TO_NEAREST(section->virtual_size, pe->header.file_alignment);
				uint32_t rs = section->size_of_raw_data;
				size_of_initialized_data += MAX(vs, rs);
			} else if (pe->header.magic == PE32PLUS_MAGIC) {
				size_of_initialized_data += TO_NEAREST(section->size_of_raw_data, pe->header.file_alignment);
			}
		}

		if (CHECK_BIT(section->characteristics, IMAGE_SCN_CNT_UNINITIALIZED_DATA)) {
			size_of_uninitialized_data += TO_NEAREST(section->virtual_size, pe->header.file_alignment);
		}
	}

	// PE files with only data can have this set to garbage. Might as well just keep it.
	if (size_of_code) {
		pe->header.base_of_code = base_of_code;
	}

	// The actual value of these of PE images in the wild varies a lot.
	// There doesn't appear to be an actual correct way of calculating these

	pe->header.base_of_data = base_of_data;
	pe->header.size_of_initialized_data = TO_NEAREST(size_of_initialized_data, pe->header.file_alignment);
	pe->header.size_of_uninitialized_data = TO_NEAREST(size_of_uninitialized_data, pe->header.file_alignment);
	pe->header.size_of_code = TO_NEAREST(size_of_code, pe->header.file_alignment);

	size_t virtual_sections_end = pe->sections[pe->header.number_of_sections - 1]->virtual_address
			+ pe->sections[pe->header.number_of_sections - 1]->virtual_size;
	pe->header.size_of_image = TO_NEAREST(virtual_sections_end, pe->header.section_alignment);

	pe->header.size_of_headers = TO_NEAREST(size_of_headers, pe->header.file_alignment);

	for (uint32_t i = 0; i < pe->header.number_of_rva_and_sizes; ++i) {
		if (!pe->data_directories[i].section) {
			pe->header.data_directories[i].virtual_address = 0;
			pe->header.data_directories[i].size = 0;
		} else {
			uint32_t directory_va = pe->data_directories[i].section->virtual_address + pe->data_directories[i].offset;
			uint32_t directory_size = pe->data_directories[i].size;

			pe->header.data_directories[i].virtual_address = directory_va;
			pe->header.data_directories[i].size = directory_size;
		}
	}

	if (pe->certificate_table.size) {
		size_t size = 0;
		for (uint32_t i = 0; i < pe->certificate_table.size; ++i) {
			size += pe->certificate_table.certificates[i].length;
		}

		pe->header.data_directories[DIR_CERTIFICATE_TABLE].virtual_address = pe->certificate_table.offset;
		pe->header.data_directories[DIR_CERTIFICATE_TABLE].size = size;
	}
}
