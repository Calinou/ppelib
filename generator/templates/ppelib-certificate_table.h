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

#ifndef PPELIB_CERTIFICATE_TABLE_H
#define PPELIB_CERTIFICATE_TABLE_H

#include <stdint.h>
#include <stddef.h>

typedef struct ppelib_certificate {
{%- for f in fields %}
{%- if 'format' in f and 'string' in f.format %}
  uint8_t {{f.name}}[{{f.pe_size + 1}}];
{%- else %}
  {{f.pe_type}} {{f.name}};
{%- endif %}
{%- endfor %}
} ppelib_certificate_t;

typedef struct ppelib_certificate_table {
  size_t size;
  size_t offset;
  ppelib_certificate_t* certificates;
} ppelib_certificate_table_t;

void ppelib_print_certificate_table(const ppelib_certificate_table_t* certificate_table);

#endif /* PPELIB_CERTIFICATE_TABLE_H */
