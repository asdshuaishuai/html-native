//////////////////////////////////////////////////////////////////////////
/*
  PainterEngine Core - glTF 2.0 / GLB model parser (PX_gltf)
  Ported from cgltf 1.15 (https://github.com/jkuhlmann/cgltf), see PX_gltf.h

  cgltf - a single-file glTF 2.0 parser written in C99.
  Copyright (c) 2018-2021 Johannes Kuhlmann
  https://github.com/jkuhlmann/cgltf

  jsmn - a minimalistic JSON parser
  Copyright (c) 2010 Serge A. Zaitsev
  https://github.com/zserge/jsmn

  Both are distributed under the MIT License:

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/
//////////////////////////////////////////////////////////////////////////
#include "PX_gltf.h"

#define PX_GLTF_FLT_MAX 3.402823466e+38F
#define PX_GLTF_SIZE_MAX ((px_gltf_size)~(px_gltf_size)0)

/* PX_GLTF_JSMN_PARENT_LINKS is necessary to make parsing large structures linear in input size */
#define PX_GLTF_JSMN_PARENT_LINKS

/* PX_GLTF_JSMN_STRICT is necessary to reject invalid JSON documents */
#define PX_GLTF_JSMN_STRICT

/*
 * -- jsmn.h start --
 * Source: https://github.com/zserge/jsmn
 * License: MIT
 */
typedef enum {
	PX_GLTF_JSMN_UNDEFINED = 0,
	PX_GLTF_JSMN_OBJECT = 1,
	PX_GLTF_JSMN_ARRAY = 2,
	PX_GLTF_JSMN_STRING = 3,
	PX_GLTF_JSMN_PRIMITIVE = 4
} PX_GLTF_JSMN_TYPE;
enum PX_GLTF_JSMN_ERR {
	/* Not enough tokens were provided */
	PX_GLTF_JSMN_ERROR_NOMEM = -1,
	/* Invalid character inside JSON string */
	PX_GLTF_JSMN_ERROR_INVAL = -2,
	/* The string is not a full JSON packet, more bytes expected */
	PX_GLTF_JSMN_ERROR_PART = -3
};
typedef struct {
	PX_GLTF_JSMN_TYPE type;
	px_gltf_ssize start;
	px_gltf_ssize end;
	px_int size;
#ifdef PX_GLTF_JSMN_PARENT_LINKS
	px_int parent;
#endif
} PX_Gltf_JsmnToken;
typedef struct {
	px_gltf_size pos; /* offset in the JSON string */
	px_uint toknext; /* next token to allocate */
	px_int toksuper; /* superior token node, e.g parent object or array */
} PX_Gltf_JsmnParser;
static px_void PX_Gltf_JsmnInit(PX_Gltf_JsmnParser *parser);
static px_int PX_Gltf_JsmnParse(PX_Gltf_JsmnParser *parser, const px_char *js, px_gltf_size len, PX_Gltf_JsmnToken *tokens, px_gltf_size num_tokens);
/*
 * -- jsmn.h end --
 */

#define PX_GLTF_GLB_HEADER_SIZE 12
#define PX_GLTF_GLB_CHUNK_HEADER_SIZE 8
#define PX_GLTF_GLB_VERSION 2
#define PX_GLTF_GLB_MAGIC 0x46546C67
#define PX_GLTF_GLB_MAGIC_JSON_CHUNK 0x4E4F534A
#define PX_GLTF_GLB_MAGIC_BIN_CHUNK 0x004E4942

#ifndef PX_GLTF_VALIDATE_ENABLE_ASSERTS
#define PX_GLTF_VALIDATE_ENABLE_ASSERTS 0
#endif

static px_void* PX_Gltf_DefaultAlloc(px_void* user, px_gltf_size size)
{
	px_memorypool* mp = (px_memorypool*)user;
	if (mp == PX_NULL)
	{
		return PX_NULL;
	}
	if (size == 0)
	{
		size = 1;
	}
	return MP_Malloc(mp, (px_uint)size);
}

static px_void PX_Gltf_DefaultFree(px_void* user, px_void* ptr)
{
	px_memorypool* mp = (px_memorypool*)user;
	if (mp == PX_NULL || ptr == PX_NULL)
	{
		return;
	}
	MP_Free(mp, ptr);
}

/* fill in default (memory pool based) allocator when the caller did not provide one */
static px_bool PX_Gltf_FixupOptions(px_memorypool* mp, const PX_Gltf_Options* options, PX_Gltf_Options* fixed_options)
{
	if (options)
	{
		*fixed_options = *options;
	}
	else
	{
		PX_memset(fixed_options, 0, sizeof(PX_Gltf_Options));
	}

	if (fixed_options->memory.alloc_func == PX_NULL && fixed_options->memory.free_func == PX_NULL)
	{
		if (mp == PX_NULL)
		{
			return PX_FALSE;
		}
		fixed_options->memory.alloc_func = &PX_Gltf_DefaultAlloc;
		fixed_options->memory.free_func = &PX_Gltf_DefaultFree;
		fixed_options->memory.user_data = mp;
	}
	else if (fixed_options->memory.alloc_func == PX_NULL || fixed_options->memory.free_func == PX_NULL)
	{
		/* alloc_func and free_func must be provided together */
		return PX_FALSE;
	}

	return PX_TRUE;
}

static px_int PX_Gltf_Strncmp(const px_char* s1, const px_char* s2, px_gltf_size n)
{
	while (n && *s1 && (*s1 == *s2))
	{
		++s1;
		++s2;
		--n;
	}
	if (n == 0)
	{
		return 0;
	}
	return (px_int)(*(const px_uchar*)s1) - (px_int)(*(const px_uchar*)s2);
}

static const px_char* PX_Gltf_Strrchr(const px_char* s, px_char ch)
{
	const px_char* last = PX_NULL;
	while (*s)
	{
		if (*s == ch)
		{
			last = s;
		}
		++s;
	}
	return last;
}

static px_gltf_size PX_Gltf_Strcspn(const px_char* s, const px_char* reject)
{
	const px_char* p = s;
	while (*p)
	{
		const px_char* r = reject;
		while (*r)
		{
			if (*p == *r)
			{
				return (px_gltf_size)(p - s);
			}
			++r;
		}
		++p;
	}
	return (px_gltf_size)(p - s);
}

/* JSON number -> integer (replacement for atoi/atoll, bounded by len) */
static px_int64 PX_Gltf_StringToInt64(const px_char* str, px_int len)
{
	px_int i = 0;
	px_bool negative = PX_FALSE;
	px_int64 value = 0;

	if (i < len && (str[i] == '-' || str[i] == '+'))
	{
		negative = (str[i] == '-');
		++i;
	}
	while (i < len && str[i] >= '0' && str[i] <= '9')
	{
		value = value * 10 + (str[i] - '0');
		++i;
	}
	return negative ? -value : value;
}

/* JSON number -> double (replacement for atof, bounded by len, supports fraction and exponent) */
static px_double PX_Gltf_StringToDouble(const px_char* str, px_int len)
{
	px_int i = 0;
	px_bool negative = PX_FALSE;
	px_double mantissa = 0;
	px_int exponent = 0;
	px_int digits = 0;

	if (i < len && (str[i] == '-' || str[i] == '+'))
	{
		negative = (str[i] == '-');
		++i;
	}
	while (i < len && str[i] >= '0' && str[i] <= '9')
	{
		if (digits < 17)
		{
			mantissa = mantissa * 10 + (str[i] - '0');
			++digits;
		}
		else
		{
			++exponent;
		}
		++i;
	}
	if (i < len && str[i] == '.')
	{
		++i;
		while (i < len && str[i] >= '0' && str[i] <= '9')
		{
			if (digits < 17)
			{
				mantissa = mantissa * 10 + (str[i] - '0');
				++digits;
				--exponent;
			}
			++i;
		}
	}
	if (i < len && (str[i] == 'e' || str[i] == 'E'))
	{
		px_int exp_value = 0;
		px_bool exp_negative = PX_FALSE;
		++i;
		if (i < len && (str[i] == '-' || str[i] == '+'))
		{
			exp_negative = (str[i] == '-');
			++i;
		}
		while (i < len && str[i] >= '0' && str[i] <= '9')
		{
			if (exp_value < 10000)
			{
				exp_value = exp_value * 10 + (str[i] - '0');
			}
			++i;
		}
		exponent += exp_negative ? -exp_value : exp_value;
	}

	if (mantissa != 0 && exponent != 0)
	{
		px_double scale = 1;
		px_int n = exponent < 0 ? -exponent : exponent;
		if (n > 350)
		{
			n = 350;
		}
		while (n--)
		{
			scale *= 10;
		}
		mantissa = exponent < 0 ? mantissa / scale : mantissa * scale;
	}
	return negative ? -mantissa : mantissa;
}

static px_void* PX_Gltf_Calloc(PX_Gltf_Options* options, px_gltf_size element_size, px_gltf_size count)
{
	if (PX_GLTF_SIZE_MAX / element_size < count)
	{
		return PX_NULL;
	}
	px_void* result = options->memory.alloc_func(options->memory.user_data, element_size * count);
	if (!result)
	{
		return PX_NULL;
	}
	PX_memset(result, 0, (px_int)(element_size * count));
	return result;
}

static PX_GLTF_RESULT PX_Gltf_DefaultFileRead(const struct PX_Gltf_MemoryOptions* memory_options, const struct PX_Gltf_FileOptions* file_options, const px_char* path, px_gltf_size* size, px_void** data)
{
	/* PainterEngine core has no file system access: external resources must be supplied through PX_Gltf_FileOptions.read */
	(px_void)memory_options;
	(px_void)file_options;
	(px_void)path;
	(px_void)size;
	(px_void)data;
	return PX_GLTF_RESULT_FILE_NOT_FOUND;
}

static px_void PX_Gltf_DefaultFileRelease(const struct PX_Gltf_MemoryOptions* memory_options, const struct PX_Gltf_FileOptions* file_options, px_void* data, px_gltf_size size)
{
	(px_void)file_options;
	(px_void)size;
	px_void (*memfree)(px_void*, px_void*) = memory_options->free_func ? memory_options->free_func : &PX_Gltf_DefaultFree;
	memfree(memory_options->user_data, data);
}

static PX_GLTF_RESULT PX_Gltf_ParseJson(PX_Gltf_Options* options, const px_byte* json_chunk, px_gltf_size size, PX_Gltf_Data** out_data);

PX_GLTF_RESULT PX_GltfParse(px_memorypool* mp, const PX_Gltf_Options* options, const px_void* data, px_gltf_size size, PX_Gltf_Data** out_data)
{
	PX_Gltf_Options fixed_options;

	if (data == PX_NULL || out_data == PX_NULL)
	{
		return PX_GLTF_RESULT_INVALID_OPTIONS;
	}

	if (size < PX_GLTF_GLB_HEADER_SIZE)
	{
		return PX_GLTF_RESULT_DATA_TOO_SHORT;
	}

	if (!PX_Gltf_FixupOptions(mp, options, &fixed_options))
	{
		return PX_GLTF_RESULT_INVALID_OPTIONS;
	}

	px_dword tmp;
	// Magic
	PX_memcpy(&tmp, data, (px_int)(4));
	if (tmp != PX_GLTF_GLB_MAGIC)
	{
		if (fixed_options.type == PX_GLTF_FILE_TYPE_INVALID)
		{
			fixed_options.type = PX_GLTF_FILE_TYPE_GLTF;
		}
		else if (fixed_options.type == PX_GLTF_FILE_TYPE_GLB)
		{
			return PX_GLTF_RESULT_UNKNOWN_FORMAT;
		}
	}

	if (fixed_options.type == PX_GLTF_FILE_TYPE_GLTF)
	{
		PX_GLTF_RESULT json_result = PX_Gltf_ParseJson(&fixed_options, (const px_byte*)data, size, out_data);
		if (json_result != PX_GLTF_RESULT_SUCCESS)
		{
			return json_result;
		}

		(*out_data)->file_type = PX_GLTF_FILE_TYPE_GLTF;

		return PX_GLTF_RESULT_SUCCESS;
	}

	const px_byte* ptr = (const px_byte*)data;
	// Version
	PX_memcpy(&tmp, ptr + 4, (px_int)(4));
	px_dword version = tmp;
	if (version != PX_GLTF_GLB_VERSION)
	{
		return version < PX_GLTF_GLB_VERSION ? PX_GLTF_RESULT_LEGACY_GLTF : PX_GLTF_RESULT_UNKNOWN_FORMAT;
	}

	// Total length
	PX_memcpy(&tmp, ptr + 8, (px_int)(4));
	if (tmp > size)
	{
		return PX_GLTF_RESULT_DATA_TOO_SHORT;
	}

	const px_byte* json_chunk = ptr + PX_GLTF_GLB_HEADER_SIZE;

	if (PX_GLTF_GLB_HEADER_SIZE + PX_GLTF_GLB_CHUNK_HEADER_SIZE > size)
	{
		return PX_GLTF_RESULT_DATA_TOO_SHORT;
	}

	// JSON chunk: length
	px_dword json_length;
	PX_memcpy(&json_length, json_chunk, (px_int)(4));
	if (json_length > size - PX_GLTF_GLB_HEADER_SIZE - PX_GLTF_GLB_CHUNK_HEADER_SIZE)
	{
		return PX_GLTF_RESULT_DATA_TOO_SHORT;
	}

	// JSON chunk: magic
	PX_memcpy(&tmp, json_chunk + 4, (px_int)(4));
	if (tmp != PX_GLTF_GLB_MAGIC_JSON_CHUNK)
	{
		return PX_GLTF_RESULT_UNKNOWN_FORMAT;
	}

	json_chunk += PX_GLTF_GLB_CHUNK_HEADER_SIZE;

	const px_void* bin = PX_NULL;
	px_gltf_size bin_size = 0;

	if (PX_GLTF_GLB_CHUNK_HEADER_SIZE <= size - PX_GLTF_GLB_HEADER_SIZE - PX_GLTF_GLB_CHUNK_HEADER_SIZE - json_length)
	{
		// We can read another chunk
		const px_byte* bin_chunk = json_chunk + json_length;

		// Bin chunk: length
		px_dword bin_length;
		PX_memcpy(&bin_length, bin_chunk, (px_int)(4));
		if (bin_length > size - PX_GLTF_GLB_HEADER_SIZE - PX_GLTF_GLB_CHUNK_HEADER_SIZE - json_length - PX_GLTF_GLB_CHUNK_HEADER_SIZE)
		{
			return PX_GLTF_RESULT_DATA_TOO_SHORT;
		}

		// Bin chunk: magic
		PX_memcpy(&tmp, bin_chunk + 4, (px_int)(4));
		if (tmp != PX_GLTF_GLB_MAGIC_BIN_CHUNK)
		{
			return PX_GLTF_RESULT_UNKNOWN_FORMAT;
		}

		bin_chunk += PX_GLTF_GLB_CHUNK_HEADER_SIZE;

		bin = bin_chunk;
		bin_size = bin_length;
	}

	PX_GLTF_RESULT json_result = PX_Gltf_ParseJson(&fixed_options, json_chunk, json_length, out_data);
	if (json_result != PX_GLTF_RESULT_SUCCESS)
	{
		return json_result;
	}

	(*out_data)->file_type = PX_GLTF_FILE_TYPE_GLB;
	(*out_data)->bin = bin;
	(*out_data)->bin_size = bin_size;

	return PX_GLTF_RESULT_SUCCESS;
}

static px_void PX_Gltf_CombinePaths(px_char* path, const px_char* base, const px_char* uri)
{
	const px_char* s0 = PX_Gltf_Strrchr(base, '/');
	const px_char* s1 = PX_Gltf_Strrchr(base, '\\');
	const px_char* slash = s0 ? (s1 && s1 > s0 ? s1 : s0) : s1;

	if (slash)
	{
		px_gltf_size prefix = slash - base + 1;

		PX_memcpy(path, base, (px_int)prefix);
		PX_memcpy(path + prefix, uri, PX_strlen(uri) + 1);
	}
	else
	{
		PX_memcpy(path, uri, PX_strlen(uri) + 1);
	}
}

static PX_GLTF_RESULT PX_Gltf_LoadBufferFile(const PX_Gltf_Options* options, px_gltf_size size, const px_char* uri, const px_char* gltf_path, px_void** out_data)
{
	px_void* (*memory_alloc)(px_void*, px_gltf_size) = options->memory.alloc_func ? options->memory.alloc_func : &PX_Gltf_DefaultAlloc;
	px_void (*memory_free)(px_void*, px_void*) = options->memory.free_func ? options->memory.free_func : &PX_Gltf_DefaultFree;
	PX_GLTF_RESULT (*file_read)(const struct PX_Gltf_MemoryOptions*, const struct PX_Gltf_FileOptions*, const px_char*, px_gltf_size*, px_void**) = options->file.read ? options->file.read : &PX_Gltf_DefaultFileRead;

	px_char* path = (px_char*)memory_alloc(options->memory.user_data, PX_strlen(uri) + PX_strlen(gltf_path) + 1);
	if (!path)
	{
		return PX_GLTF_RESULT_OUT_OF_MEMORY;
	}

	PX_Gltf_CombinePaths(path, gltf_path, uri);

	// after combining, the tail of the resulting path is a uri; decode_uri converts it into path
	PX_GltfDecodeUri(path + PX_strlen(path) - PX_strlen(uri));

	px_void* file_data = PX_NULL;
	PX_GLTF_RESULT result = file_read(&options->memory, &options->file, path, &size, &file_data);

	memory_free(options->memory.user_data, path);

	*out_data = (result == PX_GLTF_RESULT_SUCCESS) ? file_data : PX_NULL;

	return result;
}

PX_GLTF_RESULT PX_GltfLoadBufferBase64(px_memorypool* mp, const PX_Gltf_Options* options, px_gltf_size size, const px_char* base64, px_void** out_data)
{
	PX_Gltf_Options fixed_options;
	px_void* (*memory_alloc)(px_void*, px_gltf_size);
	px_void (*memory_free)(px_void*, px_void*);
	px_uchar* data;
	px_uint buffer = 0;
	px_uint buffer_bits = 0;

	if (base64 == PX_NULL || out_data == PX_NULL || !PX_Gltf_FixupOptions(mp, options, &fixed_options))
	{
		return PX_GLTF_RESULT_INVALID_OPTIONS;
	}
	memory_alloc = fixed_options.memory.alloc_func;
	memory_free = fixed_options.memory.free_func;

	data = (px_uchar*)memory_alloc(fixed_options.memory.user_data, size);
	if (!data)
	{
		return PX_GLTF_RESULT_OUT_OF_MEMORY;
	}

	for (px_gltf_size i = 0; i < size; ++i)
	{
		while (buffer_bits < 8)
		{
			px_char ch = *base64++;

			px_int index =
				(px_uint)(ch - 'A') < 26 ? (ch - 'A') :
				(px_uint)(ch - 'a') < 26 ? (ch - 'a') + 26 :
				(px_uint)(ch - '0') < 10 ? (ch - '0') + 52 :
				ch == '+' ? 62 :
				ch == '/' ? 63 :
				-1;

			if (index < 0)
			{
				memory_free(fixed_options.memory.user_data, data);
				return PX_GLTF_RESULT_IO_ERROR;
			}

			buffer = (buffer << 6) | index;
			buffer_bits += 6;
		}

		data[i] = (px_uchar)(buffer >> (buffer_bits - 8));
		buffer_bits -= 8;
	}

	*out_data = data;

	return PX_GLTF_RESULT_SUCCESS;
}

static px_int PX_Gltf_Unhex(px_char ch)
{
	return
		(px_uint)(ch - '0') < 10 ? (ch - '0') :
		(px_uint)(ch - 'A') < 6 ? (ch - 'A') + 10 :
		(px_uint)(ch - 'a') < 6 ? (ch - 'a') + 10 :
		-1;
}

px_gltf_size PX_GltfDecodeString(px_char* string)
{
	px_char* read = string + PX_Gltf_Strcspn(string, "\\");
	if (*read == 0)
	{
		return read - string;
	}
	px_char* write = string;
	px_char* last = string;

	for (;;)
	{
		// Copy characters since last escaped sequence
		px_gltf_size written = read - last;
		PX_memmove(write, last, (px_int)(written));
		write += written;

		if (*read++ == 0)
		{
			break;
		}

		// jsmn already checked that all escape sequences are valid
		switch (*read++)
		{
		case '\"': *write++ = '\"'; break;
		case '/':  *write++ = '/';  break;
		case '\\': *write++ = '\\'; break;
		case 'b':  *write++ = '\b'; break;
		case 'f':  *write++ = '\f'; break;
		case 'r':  *write++ = '\r'; break;
		case 'n':  *write++ = '\n'; break;
		case 't':  *write++ = '\t'; break;
		case 'u':
		{
			// UCS-2 codepoint \uXXXX to UTF-8
			px_int character = 0;
			for (px_gltf_size i = 0; i < 4; ++i)
			{
				character = (character << 4) + PX_Gltf_Unhex(*read++);
			}

			if (character <= 0x7F)
			{
				*write++ = character & 0xFF;
			}
			else if (character <= 0x7FF)
			{
				*write++ = 0xC0 | ((character >> 6) & 0xFF);
				*write++ = 0x80 | (character & 0x3F);
			}
			else
			{
				*write++ = 0xE0 | ((character >> 12) & 0xFF);
				*write++ = 0x80 | ((character >> 6) & 0x3F);
				*write++ = 0x80 | (character & 0x3F);
			}
			break;
		}
		default:
			break;
		}

		last = read;
		read += PX_Gltf_Strcspn(read, "\\");
	}

	*write = 0;
	return write - string;
}

px_gltf_size PX_GltfDecodeUri(px_char* uri)
{
	px_char* write = uri;
	px_char* i = uri;

	while (*i)
	{
		if (*i == '%')
		{
			px_int ch1 = PX_Gltf_Unhex(i[1]);

			if (ch1 >= 0)
			{
				px_int ch2 = PX_Gltf_Unhex(i[2]);

				if (ch2 >= 0)
				{
					*write++ = (px_char)(ch1 * 16 + ch2);
					i += 3;
					continue;
				}
			}
		}

		*write++ = *i++;
	}

	*write = 0;
	return write - uri;
}

PX_GLTF_RESULT PX_GltfLoadBuffers(const PX_Gltf_Options* options, PX_Gltf_Data* data, const px_char* gltf_path)
{
	PX_Gltf_Options fixed_options;

	if (data == PX_NULL)
	{
		return PX_GLTF_RESULT_INVALID_OPTIONS;
	}

	/* fall back to the allocator / file callbacks recorded at parse time */
	if (options)
	{
		fixed_options = *options;
	}
	else
	{
		PX_memset(&fixed_options, 0, sizeof(PX_Gltf_Options));
	}
	if (fixed_options.memory.alloc_func == PX_NULL || fixed_options.memory.free_func == PX_NULL)
	{
		fixed_options.memory = data->memory;
	}
	if (fixed_options.file.read == PX_NULL && fixed_options.file.release == PX_NULL)
	{
		fixed_options.file = data->file;
	}
	if (fixed_options.memory.alloc_func == PX_NULL || fixed_options.memory.free_func == PX_NULL)
	{
		return PX_GLTF_RESULT_INVALID_OPTIONS;
	}
	/* remember the file callbacks so that PX_GltfFree releases file buffers the same way they were read */
	if (data->file.read == PX_NULL && data->file.release == PX_NULL)
	{
		data->file = fixed_options.file;
	}

	if (data->buffers_count && data->buffers[0].data == PX_NULL && data->buffers[0].uri == PX_NULL && data->bin)
	{
		if (data->bin_size < data->buffers[0].size)
		{
			return PX_GLTF_RESULT_DATA_TOO_SHORT;
		}

		data->buffers[0].data = (px_void*)data->bin;
		data->buffers[0].data_free_method = PX_GLTF_DATA_FREE_METHOD_NONE;
	}

	for (px_gltf_size i = 0; i < data->buffers_count; ++i)
	{
		if (data->buffers[i].data)
		{
			continue;
		}

		const px_char* uri = data->buffers[i].uri;

		if (uri == PX_NULL)
		{
			continue;
		}

		if (PX_Gltf_Strncmp(uri, "data:", 5) == 0)
		{
			const px_char* comma = PX_strchr(uri, ',');

			if (comma && comma - uri >= 7 && PX_Gltf_Strncmp(comma - 7, ";base64", 7) == 0)
			{
				PX_GLTF_RESULT res = PX_GltfLoadBufferBase64(PX_NULL, &fixed_options, data->buffers[i].size, comma + 1, &data->buffers[i].data);
				data->buffers[i].data_free_method = PX_GLTF_DATA_FREE_METHOD_MEMORY_FREE;

				if (res != PX_GLTF_RESULT_SUCCESS)
				{
					return res;
				}
			}
			else
			{
				return PX_GLTF_RESULT_UNKNOWN_FORMAT;
			}
		}
		else if (PX_strstr(uri, "://") == PX_NULL && gltf_path)
		{
			PX_GLTF_RESULT res = PX_Gltf_LoadBufferFile(&fixed_options, data->buffers[i].size, uri, gltf_path, &data->buffers[i].data);
			data->buffers[i].data_free_method = PX_GLTF_DATA_FREE_METHOD_FILE_RELEASE;

			if (res != PX_GLTF_RESULT_SUCCESS)
			{
				return res;
			}
		}
		else
		{
			return PX_GLTF_RESULT_UNKNOWN_FORMAT;
		}
	}

	return PX_GLTF_RESULT_SUCCESS;
}

static px_gltf_size PX_Gltf_CalcIndexBound(PX_Gltf_BufferView* buffer_view, px_gltf_size offset, PX_GLTF_COMPONENT_TYPE component_type, px_gltf_size count)
{
	px_char* data = (px_char*)buffer_view->buffer->data + offset + buffer_view->offset;
	px_gltf_size bound = 0;

	switch (component_type)
	{
	case PX_GLTF_COMPONENT_TYPE_R_8U:
		for (px_gltf_size i = 0; i < count; ++i)
		{
			px_gltf_size v = ((px_uchar*)data)[i];
			bound = bound > v ? bound : v;
		}
		break;

	case PX_GLTF_COMPONENT_TYPE_R_16U:
		for (px_gltf_size i = 0; i < count; ++i)
		{
			px_gltf_size v = ((px_word*)data)[i];
			bound = bound > v ? bound : v;
		}
		break;

	case PX_GLTF_COMPONENT_TYPE_R_32U:
		for (px_gltf_size i = 0; i < count; ++i)
		{
			px_gltf_size v = ((px_uint*)data)[i];
			bound = bound > v ? bound : v;
		}
		break;

	default:
		;
	}

	return bound;
}

#if PX_GLTF_VALIDATE_ENABLE_ASSERTS
#define PX_GLTF_ASSERT_IF(cond, result) PX_ASSERTIF(cond); if (cond) return result;
#else
#define PX_GLTF_ASSERT_IF(cond, result) if (cond) return result;
#endif

PX_GLTF_RESULT PX_GltfValidate(PX_Gltf_Data* data)
{
	for (px_gltf_size i = 0; i < data->accessors_count; ++i)
	{
		PX_Gltf_Accessor* accessor = &data->accessors[i];

		PX_GLTF_ASSERT_IF(data->accessors[i].component_type == PX_GLTF_COMPONENT_TYPE_INVALID, PX_GLTF_RESULT_INVALID_GLTF);
		PX_GLTF_ASSERT_IF(data->accessors[i].type == PX_GLTF_TYPE_INVALID, PX_GLTF_RESULT_INVALID_GLTF);

		px_gltf_size element_size = PX_GltfCalcSize(accessor->type, accessor->component_type);

		if (accessor->buffer_view)
		{
			px_gltf_size req_size = accessor->offset + accessor->stride * (accessor->count - 1) + element_size;

			PX_GLTF_ASSERT_IF(accessor->buffer_view->size < req_size, PX_GLTF_RESULT_DATA_TOO_SHORT);
		}

		if (accessor->is_sparse)
		{
			PX_Gltf_AccessorSparse* sparse = &accessor->sparse;

			px_gltf_size indices_component_size = PX_GltfComponentSize(sparse->indices_component_type);
			px_gltf_size indices_req_size = sparse->indices_byte_offset + indices_component_size * sparse->count;
			px_gltf_size values_req_size = sparse->values_byte_offset + element_size * sparse->count;

			PX_GLTF_ASSERT_IF(sparse->indices_buffer_view->size < indices_req_size ||
							sparse->values_buffer_view->size < values_req_size, PX_GLTF_RESULT_DATA_TOO_SHORT);

			PX_GLTF_ASSERT_IF(sparse->indices_component_type != PX_GLTF_COMPONENT_TYPE_R_8U &&
							sparse->indices_component_type != PX_GLTF_COMPONENT_TYPE_R_16U &&
							sparse->indices_component_type != PX_GLTF_COMPONENT_TYPE_R_32U, PX_GLTF_RESULT_INVALID_GLTF);

			if (sparse->indices_buffer_view->buffer->data)
			{
				px_gltf_size index_bound = PX_Gltf_CalcIndexBound(sparse->indices_buffer_view, sparse->indices_byte_offset, sparse->indices_component_type, sparse->count);

				PX_GLTF_ASSERT_IF(index_bound >= accessor->count, PX_GLTF_RESULT_DATA_TOO_SHORT);
			}
		}
	}

	for (px_gltf_size i = 0; i < data->buffer_views_count; ++i)
	{
		px_gltf_size req_size = data->buffer_views[i].offset + data->buffer_views[i].size;

		PX_GLTF_ASSERT_IF(data->buffer_views[i].buffer && data->buffer_views[i].buffer->size < req_size, PX_GLTF_RESULT_DATA_TOO_SHORT);

		if (data->buffer_views[i].has_meshopt_compression)
		{
			PX_Gltf_MeshoptCompression* mc = &data->buffer_views[i].meshopt_compression;

			PX_GLTF_ASSERT_IF(mc->buffer == PX_NULL || mc->buffer->size < mc->offset + mc->size, PX_GLTF_RESULT_DATA_TOO_SHORT);

			PX_GLTF_ASSERT_IF(data->buffer_views[i].stride && mc->stride != data->buffer_views[i].stride, PX_GLTF_RESULT_INVALID_GLTF);

			PX_GLTF_ASSERT_IF(data->buffer_views[i].size != mc->stride * mc->count, PX_GLTF_RESULT_INVALID_GLTF);

			PX_GLTF_ASSERT_IF(mc->mode == PX_GLTF_MESHOPT_COMPRESSION_MODE_INVALID, PX_GLTF_RESULT_INVALID_GLTF);

			PX_GLTF_ASSERT_IF(mc->mode == PX_GLTF_MESHOPT_COMPRESSION_MODE_ATTRIBUTES && !(mc->stride % 4 == 0 && mc->stride <= 256), PX_GLTF_RESULT_INVALID_GLTF);

			PX_GLTF_ASSERT_IF(mc->mode == PX_GLTF_MESHOPT_COMPRESSION_MODE_TRIANGLES && mc->count % 3 != 0, PX_GLTF_RESULT_INVALID_GLTF);

			PX_GLTF_ASSERT_IF((mc->mode == PX_GLTF_MESHOPT_COMPRESSION_MODE_TRIANGLES || mc->mode == PX_GLTF_MESHOPT_COMPRESSION_MODE_INDICES) && mc->stride != 2 && mc->stride != 4, PX_GLTF_RESULT_INVALID_GLTF);

			PX_GLTF_ASSERT_IF((mc->mode == PX_GLTF_MESHOPT_COMPRESSION_MODE_TRIANGLES || mc->mode == PX_GLTF_MESHOPT_COMPRESSION_MODE_INDICES) && mc->filter != PX_GLTF_MESHOPT_COMPRESSION_FILTER_NONE, PX_GLTF_RESULT_INVALID_GLTF);

			PX_GLTF_ASSERT_IF(mc->filter == PX_GLTF_MESHOPT_COMPRESSION_FILTER_OCTAHEDRAL && mc->stride != 4 && mc->stride != 8, PX_GLTF_RESULT_INVALID_GLTF);
			PX_GLTF_ASSERT_IF(mc->filter == PX_GLTF_MESHOPT_COMPRESSION_FILTER_QUATERNION && mc->stride != 8, PX_GLTF_RESULT_INVALID_GLTF);
			PX_GLTF_ASSERT_IF(mc->filter == PX_GLTF_MESHOPT_COMPRESSION_FILTER_COLOR && mc->stride != 4 && mc->stride != 8, PX_GLTF_RESULT_INVALID_GLTF);
		}
	}

	for (px_gltf_size i = 0; i < data->meshes_count; ++i)
	{
		if (data->meshes[i].weights)
		{
			PX_GLTF_ASSERT_IF(data->meshes[i].primitives_count && data->meshes[i].primitives[0].targets_count != data->meshes[i].weights_count, PX_GLTF_RESULT_INVALID_GLTF);
		}

		if (data->meshes[i].target_names)
		{
			PX_GLTF_ASSERT_IF(data->meshes[i].primitives_count && data->meshes[i].primitives[0].targets_count != data->meshes[i].target_names_count, PX_GLTF_RESULT_INVALID_GLTF);
		}

		for (px_gltf_size j = 0; j < data->meshes[i].primitives_count; ++j)
		{
			PX_GLTF_ASSERT_IF(data->meshes[i].primitives[j].type == PX_GLTF_PRIMITIVE_TYPE_INVALID, PX_GLTF_RESULT_INVALID_GLTF);
			PX_GLTF_ASSERT_IF(data->meshes[i].primitives[j].targets_count != data->meshes[i].primitives[0].targets_count, PX_GLTF_RESULT_INVALID_GLTF);

			PX_GLTF_ASSERT_IF(data->meshes[i].primitives[j].attributes_count == 0, PX_GLTF_RESULT_INVALID_GLTF);

			PX_Gltf_Accessor* first = data->meshes[i].primitives[j].attributes[0].data;

			PX_GLTF_ASSERT_IF(first->count == 0, PX_GLTF_RESULT_INVALID_GLTF);

			for (px_gltf_size k = 0; k < data->meshes[i].primitives[j].attributes_count; ++k)
			{
				PX_GLTF_ASSERT_IF(data->meshes[i].primitives[j].attributes[k].data->count != first->count, PX_GLTF_RESULT_INVALID_GLTF);
			}

			for (px_gltf_size k = 0; k < data->meshes[i].primitives[j].targets_count; ++k)
			{
				for (px_gltf_size m = 0; m < data->meshes[i].primitives[j].targets[k].attributes_count; ++m)
				{
					PX_GLTF_ASSERT_IF(data->meshes[i].primitives[j].targets[k].attributes[m].data->count != first->count, PX_GLTF_RESULT_INVALID_GLTF);
				}
			}

			PX_Gltf_Accessor* indices = data->meshes[i].primitives[j].indices;

			PX_GLTF_ASSERT_IF(indices &&
				indices->component_type != PX_GLTF_COMPONENT_TYPE_R_8U &&
				indices->component_type != PX_GLTF_COMPONENT_TYPE_R_16U &&
				indices->component_type != PX_GLTF_COMPONENT_TYPE_R_32U, PX_GLTF_RESULT_INVALID_GLTF);

			PX_GLTF_ASSERT_IF(indices && indices->type != PX_GLTF_TYPE_SCALAR, PX_GLTF_RESULT_INVALID_GLTF);
			PX_GLTF_ASSERT_IF(indices && indices->stride != PX_GltfComponentSize(indices->component_type), PX_GLTF_RESULT_INVALID_GLTF);

			if (indices && indices->buffer_view && indices->buffer_view->buffer->data)
			{
				px_gltf_size index_bound = PX_Gltf_CalcIndexBound(indices->buffer_view, indices->offset, indices->component_type, indices->count);

				PX_GLTF_ASSERT_IF(index_bound >= first->count, PX_GLTF_RESULT_DATA_TOO_SHORT);
			}

			for (px_gltf_size k = 0; k < data->meshes[i].primitives[j].mappings_count; ++k)
			{
				PX_GLTF_ASSERT_IF(data->meshes[i].primitives[j].mappings[k].variant >= data->variants_count, PX_GLTF_RESULT_INVALID_GLTF);
			}
		}
	}

	for (px_gltf_size i = 0; i < data->nodes_count; ++i)
	{
		if (data->nodes[i].weights && data->nodes[i].mesh)
		{
			PX_GLTF_ASSERT_IF(data->nodes[i].mesh->primitives_count && data->nodes[i].mesh->primitives[0].targets_count != data->nodes[i].weights_count, PX_GLTF_RESULT_INVALID_GLTF);
		}

		if (data->nodes[i].has_mesh_gpu_instancing)
		{
			PX_GLTF_ASSERT_IF(data->nodes[i].mesh == PX_NULL, PX_GLTF_RESULT_INVALID_GLTF);
			PX_GLTF_ASSERT_IF(data->nodes[i].mesh_gpu_instancing.attributes_count == 0, PX_GLTF_RESULT_INVALID_GLTF);

			PX_Gltf_Accessor* first = data->nodes[i].mesh_gpu_instancing.attributes[0].data;

			for (px_gltf_size k = 0; k < data->nodes[i].mesh_gpu_instancing.attributes_count; ++k)
			{
				PX_GLTF_ASSERT_IF(data->nodes[i].mesh_gpu_instancing.attributes[k].data->count != first->count, PX_GLTF_RESULT_INVALID_GLTF);
			}
		}
	}

	for (px_gltf_size i = 0; i < data->nodes_count; ++i)
	{
		PX_Gltf_Node* p1 = data->nodes[i].parent;
		PX_Gltf_Node* p2 = p1 ? p1->parent : PX_NULL;

		while (p1 && p2)
		{
			PX_GLTF_ASSERT_IF(p1 == p2, PX_GLTF_RESULT_INVALID_GLTF);

			p1 = p1->parent;
			p2 = p2->parent ? p2->parent->parent : PX_NULL;
		}
	}

	for (px_gltf_size i = 0; i < data->scenes_count; ++i)
	{
		for (px_gltf_size j = 0; j < data->scenes[i].nodes_count; ++j)
		{
			PX_GLTF_ASSERT_IF(data->scenes[i].nodes[j]->parent, PX_GLTF_RESULT_INVALID_GLTF);
		}
	}

	for (px_gltf_size i = 0; i < data->animations_count; ++i)
	{
		for (px_gltf_size j = 0; j < data->animations[i].channels_count; ++j)
		{
			PX_Gltf_AnimationChannel* channel = &data->animations[i].channels[j];

			if (!channel->target_node)
			{
				continue;
			}

			px_gltf_size components = 1;

			if (channel->target_path == PX_GLTF_ANIMATION_PATH_TYPE_WEIGHTS)
			{
				PX_GLTF_ASSERT_IF(!channel->target_node->mesh || !channel->target_node->mesh->primitives_count, PX_GLTF_RESULT_INVALID_GLTF);

				components = channel->target_node->mesh->primitives[0].targets_count;
			}

			px_gltf_size values = channel->sampler->interpolation == PX_GLTF_INTERPOLATION_TYPE_CUBIC_SPLINE ? 3 : 1;

			PX_GLTF_ASSERT_IF(channel->sampler->input->count * components * values != channel->sampler->output->count, PX_GLTF_RESULT_INVALID_GLTF);
		}
	}

	for (px_gltf_size i = 0; i < data->variants_count; ++i)
	{
		PX_GLTF_ASSERT_IF(!data->variants[i].name, PX_GLTF_RESULT_INVALID_GLTF);
	}

	return PX_GLTF_RESULT_SUCCESS;
}

PX_GLTF_RESULT PX_GltfCopyExtrasJson(const PX_Gltf_Data* data, const PX_Gltf_Extras* extras, px_char* dest, px_gltf_size* dest_size)
{
	px_gltf_size json_size = extras->end_offset - extras->start_offset;

	if (!dest)
	{
		if (dest_size)
		{
			*dest_size = json_size + 1;
			return PX_GLTF_RESULT_SUCCESS;
		}
		return PX_GLTF_RESULT_INVALID_OPTIONS;
	}

	if (*dest_size + 1 < json_size)
	{
		PX_memcpy(dest, data->json + extras->start_offset, (px_int)(*dest_size - 1));
		dest[*dest_size - 1] = 0;
	}
	else
	{
		PX_memcpy(dest, data->json + extras->start_offset, (px_int)(json_size));
		dest[json_size] = 0;
	}

	return PX_GLTF_RESULT_SUCCESS;
}

static px_void PX_Gltf_FreeExtras(PX_Gltf_Data* data, PX_Gltf_Extras* extras)
{
	data->memory.free_func(data->memory.user_data, extras->data);
}

static px_void PX_Gltf_FreeExtensions(PX_Gltf_Data* data, PX_Gltf_Extension* extensions, px_gltf_size extensions_count)
{
	for (px_gltf_size i = 0; i < extensions_count; ++i)
	{
		data->memory.free_func(data->memory.user_data, extensions[i].name);
		data->memory.free_func(data->memory.user_data, extensions[i].data);
	}
	data->memory.free_func(data->memory.user_data, extensions);
}

px_void PX_GltfFree(PX_Gltf_Data* data)
{
	if (!data)
	{
		return;
	}

	px_void (*file_release)(const struct PX_Gltf_MemoryOptions*, const struct PX_Gltf_FileOptions*, px_void* data, px_gltf_size size) = data->file.release ? data->file.release : PX_Gltf_DefaultFileRelease;

	data->memory.free_func(data->memory.user_data, data->asset.copyright);
	data->memory.free_func(data->memory.user_data, data->asset.generator);
	data->memory.free_func(data->memory.user_data, data->asset.version);
	data->memory.free_func(data->memory.user_data, data->asset.min_version);

	PX_Gltf_FreeExtensions(data, data->asset.extensions, data->asset.extensions_count);
	PX_Gltf_FreeExtras(data, &data->asset.extras);

	for (px_gltf_size i = 0; i < data->accessors_count; ++i)
	{
		data->memory.free_func(data->memory.user_data, data->accessors[i].name);

		PX_Gltf_FreeExtensions(data, data->accessors[i].extensions, data->accessors[i].extensions_count);
		PX_Gltf_FreeExtras(data, &data->accessors[i].extras);
	}
	data->memory.free_func(data->memory.user_data, data->accessors);

	for (px_gltf_size i = 0; i < data->buffer_views_count; ++i)
	{
		data->memory.free_func(data->memory.user_data, data->buffer_views[i].name);
		data->memory.free_func(data->memory.user_data, data->buffer_views[i].data);

		PX_Gltf_FreeExtensions(data, data->buffer_views[i].extensions, data->buffer_views[i].extensions_count);
		PX_Gltf_FreeExtras(data, &data->buffer_views[i].extras);
	}
	data->memory.free_func(data->memory.user_data, data->buffer_views);

	for (px_gltf_size i = 0; i < data->buffers_count; ++i)
	{
		data->memory.free_func(data->memory.user_data, data->buffers[i].name);

		if (data->buffers[i].data_free_method == PX_GLTF_DATA_FREE_METHOD_FILE_RELEASE)
		{
			file_release(&data->memory, &data->file, data->buffers[i].data, data->buffers[i].size);
		}
		else if (data->buffers[i].data_free_method == PX_GLTF_DATA_FREE_METHOD_MEMORY_FREE)
		{
			data->memory.free_func(data->memory.user_data, data->buffers[i].data);
		}

		data->memory.free_func(data->memory.user_data, data->buffers[i].uri);

		PX_Gltf_FreeExtensions(data, data->buffers[i].extensions, data->buffers[i].extensions_count);
		PX_Gltf_FreeExtras(data, &data->buffers[i].extras);
	}
	data->memory.free_func(data->memory.user_data, data->buffers);

	for (px_gltf_size i = 0; i < data->meshes_count; ++i)
	{
		data->memory.free_func(data->memory.user_data, data->meshes[i].name);

		for (px_gltf_size j = 0; j < data->meshes[i].primitives_count; ++j)
		{
			for (px_gltf_size k = 0; k < data->meshes[i].primitives[j].attributes_count; ++k)
			{
				data->memory.free_func(data->memory.user_data, data->meshes[i].primitives[j].attributes[k].name);
			}

			data->memory.free_func(data->memory.user_data, data->meshes[i].primitives[j].attributes);

			for (px_gltf_size k = 0; k < data->meshes[i].primitives[j].targets_count; ++k)
			{
				for (px_gltf_size m = 0; m < data->meshes[i].primitives[j].targets[k].attributes_count; ++m)
				{
					data->memory.free_func(data->memory.user_data, data->meshes[i].primitives[j].targets[k].attributes[m].name);
				}

				data->memory.free_func(data->memory.user_data, data->meshes[i].primitives[j].targets[k].attributes);
			}

			data->memory.free_func(data->memory.user_data, data->meshes[i].primitives[j].targets);

			if (data->meshes[i].primitives[j].has_draco_mesh_compression)
			{
				for (px_gltf_size k = 0; k < data->meshes[i].primitives[j].draco_mesh_compression.attributes_count; ++k)
				{
					data->memory.free_func(data->memory.user_data, data->meshes[i].primitives[j].draco_mesh_compression.attributes[k].name);
				}

				data->memory.free_func(data->memory.user_data, data->meshes[i].primitives[j].draco_mesh_compression.attributes);
			}

			for (px_gltf_size k = 0; k < data->meshes[i].primitives[j].mappings_count; ++k)
			{
				PX_Gltf_FreeExtras(data, &data->meshes[i].primitives[j].mappings[k].extras);
			}

			data->memory.free_func(data->memory.user_data, data->meshes[i].primitives[j].mappings);

			PX_Gltf_FreeExtensions(data, data->meshes[i].primitives[j].extensions, data->meshes[i].primitives[j].extensions_count);
			PX_Gltf_FreeExtras(data, &data->meshes[i].primitives[j].extras);
		}

		data->memory.free_func(data->memory.user_data, data->meshes[i].primitives);
		data->memory.free_func(data->memory.user_data, data->meshes[i].weights);

		for (px_gltf_size j = 0; j < data->meshes[i].target_names_count; ++j)
		{
			data->memory.free_func(data->memory.user_data, data->meshes[i].target_names[j]);
		}

		PX_Gltf_FreeExtensions(data, data->meshes[i].extensions, data->meshes[i].extensions_count);
		PX_Gltf_FreeExtras(data, &data->meshes[i].extras);

		data->memory.free_func(data->memory.user_data, data->meshes[i].target_names);
	}

	data->memory.free_func(data->memory.user_data, data->meshes);

	for (px_gltf_size i = 0; i < data->materials_count; ++i)
	{
		data->memory.free_func(data->memory.user_data, data->materials[i].name);

		PX_Gltf_FreeExtensions(data, data->materials[i].extensions, data->materials[i].extensions_count);
		PX_Gltf_FreeExtras(data, &data->materials[i].extras);
	}

	data->memory.free_func(data->memory.user_data, data->materials);

	for (px_gltf_size i = 0; i < data->images_count; ++i)
	{
		data->memory.free_func(data->memory.user_data, data->images[i].name);
		data->memory.free_func(data->memory.user_data, data->images[i].uri);
		data->memory.free_func(data->memory.user_data, data->images[i].mime_type);

		PX_Gltf_FreeExtensions(data, data->images[i].extensions, data->images[i].extensions_count);
		PX_Gltf_FreeExtras(data, &data->images[i].extras);
	}

	data->memory.free_func(data->memory.user_data, data->images);

	for (px_gltf_size i = 0; i < data->textures_count; ++i)
	{
		data->memory.free_func(data->memory.user_data, data->textures[i].name);

		PX_Gltf_FreeExtensions(data, data->textures[i].extensions, data->textures[i].extensions_count);
		PX_Gltf_FreeExtras(data, &data->textures[i].extras);
	}

	data->memory.free_func(data->memory.user_data, data->textures);

	for (px_gltf_size i = 0; i < data->samplers_count; ++i)
	{
		data->memory.free_func(data->memory.user_data, data->samplers[i].name);

		PX_Gltf_FreeExtensions(data, data->samplers[i].extensions, data->samplers[i].extensions_count);
		PX_Gltf_FreeExtras(data, &data->samplers[i].extras);
	}

	data->memory.free_func(data->memory.user_data, data->samplers);

	for (px_gltf_size i = 0; i < data->skins_count; ++i)
	{
		data->memory.free_func(data->memory.user_data, data->skins[i].name);
		data->memory.free_func(data->memory.user_data, data->skins[i].joints);

		PX_Gltf_FreeExtensions(data, data->skins[i].extensions, data->skins[i].extensions_count);
		PX_Gltf_FreeExtras(data, &data->skins[i].extras);
	}

	data->memory.free_func(data->memory.user_data, data->skins);

	for (px_gltf_size i = 0; i < data->cameras_count; ++i)
	{
		data->memory.free_func(data->memory.user_data, data->cameras[i].name);

		if (data->cameras[i].type == PX_GLTF_CAMERA_TYPE_PERSPECTIVE)
		{
			PX_Gltf_FreeExtras(data, &data->cameras[i].data.perspective.extras);
		}
		else if (data->cameras[i].type == PX_GLTF_CAMERA_TYPE_ORTHOGRAPHIC)
		{
			PX_Gltf_FreeExtras(data, &data->cameras[i].data.orthographic.extras);
		}

		PX_Gltf_FreeExtensions(data, data->cameras[i].extensions, data->cameras[i].extensions_count);
		PX_Gltf_FreeExtras(data, &data->cameras[i].extras);
	}

	data->memory.free_func(data->memory.user_data, data->cameras);

	for (px_gltf_size i = 0; i < data->lights_count; ++i)
	{
		data->memory.free_func(data->memory.user_data, data->lights[i].name);

		PX_Gltf_FreeExtras(data, &data->lights[i].extras);
	}

	data->memory.free_func(data->memory.user_data, data->lights);

	for (px_gltf_size i = 0; i < data->nodes_count; ++i)
	{
		data->memory.free_func(data->memory.user_data, data->nodes[i].name);
		data->memory.free_func(data->memory.user_data, data->nodes[i].children);
		data->memory.free_func(data->memory.user_data, data->nodes[i].weights);

		if (data->nodes[i].has_mesh_gpu_instancing)
		{
			for (px_gltf_size j = 0; j < data->nodes[i].mesh_gpu_instancing.attributes_count; ++j)
			{
				data->memory.free_func(data->memory.user_data, data->nodes[i].mesh_gpu_instancing.attributes[j].name);
			}

			data->memory.free_func(data->memory.user_data, data->nodes[i].mesh_gpu_instancing.attributes);
		}

		PX_Gltf_FreeExtensions(data, data->nodes[i].extensions, data->nodes[i].extensions_count);
		PX_Gltf_FreeExtras(data, &data->nodes[i].extras);
	}

	data->memory.free_func(data->memory.user_data, data->nodes);

	for (px_gltf_size i = 0; i < data->scenes_count; ++i)
	{
		data->memory.free_func(data->memory.user_data, data->scenes[i].name);
		data->memory.free_func(data->memory.user_data, data->scenes[i].nodes);

		PX_Gltf_FreeExtensions(data, data->scenes[i].extensions, data->scenes[i].extensions_count);
		PX_Gltf_FreeExtras(data, &data->scenes[i].extras);
	}

	data->memory.free_func(data->memory.user_data, data->scenes);

	for (px_gltf_size i = 0; i < data->animations_count; ++i)
	{
		data->memory.free_func(data->memory.user_data, data->animations[i].name);
		for (px_gltf_size j = 0; j <  data->animations[i].samplers_count; ++j)
		{
			PX_Gltf_FreeExtensions(data, data->animations[i].samplers[j].extensions, data->animations[i].samplers[j].extensions_count);
			PX_Gltf_FreeExtras(data, &data->animations[i].samplers[j].extras);
		}
		data->memory.free_func(data->memory.user_data, data->animations[i].samplers);

		for (px_gltf_size j = 0; j <  data->animations[i].channels_count; ++j)
		{
			PX_Gltf_FreeExtensions(data, data->animations[i].channels[j].extensions, data->animations[i].channels[j].extensions_count);
			PX_Gltf_FreeExtras(data, &data->animations[i].channels[j].extras);
		}
		data->memory.free_func(data->memory.user_data, data->animations[i].channels);

		PX_Gltf_FreeExtensions(data, data->animations[i].extensions, data->animations[i].extensions_count);
		PX_Gltf_FreeExtras(data, &data->animations[i].extras);
	}

	data->memory.free_func(data->memory.user_data, data->animations);

	for (px_gltf_size i = 0; i < data->variants_count; ++i)
	{
		data->memory.free_func(data->memory.user_data, data->variants[i].name);

		PX_Gltf_FreeExtras(data, &data->variants[i].extras);
	}

	data->memory.free_func(data->memory.user_data, data->variants);

	PX_Gltf_FreeExtensions(data, data->data_extensions, data->data_extensions_count);
	PX_Gltf_FreeExtras(data, &data->extras);

	for (px_gltf_size i = 0; i < data->extensions_used_count; ++i)
	{
		data->memory.free_func(data->memory.user_data, data->extensions_used[i]);
	}

	data->memory.free_func(data->memory.user_data, data->extensions_used);

	for (px_gltf_size i = 0; i < data->extensions_required_count; ++i)
	{
		data->memory.free_func(data->memory.user_data, data->extensions_required[i]);
	}

	data->memory.free_func(data->memory.user_data, data->extensions_required);

	file_release(&data->memory, &data->file, data->file_data, data->file_size);

	data->memory.free_func(data->memory.user_data, data);
}

px_void PX_GltfNodeTransformLocal(const PX_Gltf_Node* node, px_float* out_matrix)
{
	px_float* lm = out_matrix;

	if (node->has_matrix)
	{
		PX_memcpy(lm, node->matrix, (px_int)(sizeof(px_float) * 16));
	}
	else
	{
		px_float tx = node->translation[0];
		px_float ty = node->translation[1];
		px_float tz = node->translation[2];

		px_float qx = node->rotation[0];
		px_float qy = node->rotation[1];
		px_float qz = node->rotation[2];
		px_float qw = node->rotation[3];

		px_float sx = node->scale[0];
		px_float sy = node->scale[1];
		px_float sz = node->scale[2];

		lm[0] = (1 - 2 * qy*qy - 2 * qz*qz) * sx;
		lm[1] = (2 * qx*qy + 2 * qz*qw) * sx;
		lm[2] = (2 * qx*qz - 2 * qy*qw) * sx;
		lm[3] = 0.f;

		lm[4] = (2 * qx*qy - 2 * qz*qw) * sy;
		lm[5] = (1 - 2 * qx*qx - 2 * qz*qz) * sy;
		lm[6] = (2 * qy*qz + 2 * qx*qw) * sy;
		lm[7] = 0.f;

		lm[8] = (2 * qx*qz + 2 * qy*qw) * sz;
		lm[9] = (2 * qy*qz - 2 * qx*qw) * sz;
		lm[10] = (1 - 2 * qx*qx - 2 * qy*qy) * sz;
		lm[11] = 0.f;

		lm[12] = tx;
		lm[13] = ty;
		lm[14] = tz;
		lm[15] = 1.f;
	}
}

px_void PX_GltfNodeTransformWorld(const PX_Gltf_Node* node, px_float* out_matrix)
{
	px_float* lm = out_matrix;
	PX_GltfNodeTransformLocal(node, lm);

	const PX_Gltf_Node* parent = node->parent;

	while (parent)
	{
		px_float pm[16];
		PX_GltfNodeTransformLocal(parent, pm);

		for (px_int i = 0; i < 4; ++i)
		{
			px_float l0 = lm[i * 4 + 0];
			px_float l1 = lm[i * 4 + 1];
			px_float l2 = lm[i * 4 + 2];

			px_float r0 = l0 * pm[0] + l1 * pm[4] + l2 * pm[8];
			px_float r1 = l0 * pm[1] + l1 * pm[5] + l2 * pm[9];
			px_float r2 = l0 * pm[2] + l1 * pm[6] + l2 * pm[10];

			lm[i * 4 + 0] = r0;
			lm[i * 4 + 1] = r1;
			lm[i * 4 + 2] = r2;
		}

		lm[12] += pm[12];
		lm[13] += pm[13];
		lm[14] += pm[14];

		parent = parent->parent;
	}
}

static px_gltf_ssize PX_Gltf_ComponentReadInteger(const px_void* in, PX_GLTF_COMPONENT_TYPE component_type)
{
	switch (component_type)
	{
		case PX_GLTF_COMPONENT_TYPE_R_16:
			return *((const px_short*) in);
		case PX_GLTF_COMPONENT_TYPE_R_16U:
			return *((const px_word*) in);
		case PX_GLTF_COMPONENT_TYPE_R_32U:
			return *((const px_dword*) in);
		case PX_GLTF_COMPONENT_TYPE_R_8:
			return *((const signed char*) in);
		case PX_GLTF_COMPONENT_TYPE_R_8U:
			return *((const px_byte*) in);
		default:
			return 0;
	}
}

static px_gltf_size PX_Gltf_ComponentReadIndex(const px_void* in, PX_GLTF_COMPONENT_TYPE component_type)
{
	switch (component_type)
	{
		case PX_GLTF_COMPONENT_TYPE_R_16U:
			return *((const px_word*) in);
		case PX_GLTF_COMPONENT_TYPE_R_32U:
			return *((const px_dword*) in);
		case PX_GLTF_COMPONENT_TYPE_R_8U:
			return *((const px_byte*) in);
		default:
			return 0;
	}
}

static px_float PX_Gltf_ComponentReadFloat(const px_void* in, PX_GLTF_COMPONENT_TYPE component_type, px_bool normalized)
{
	if (component_type == PX_GLTF_COMPONENT_TYPE_R_32F)
	{
		return *((const px_float*) in);
	}

	if (normalized)
	{
		switch (component_type)
		{
			// note: glTF spec doesn't currently define normalized conversions for 32-bit integers
			case PX_GLTF_COMPONENT_TYPE_R_16:
				return *((const px_short*) in) / (px_float)32767;
			case PX_GLTF_COMPONENT_TYPE_R_16U:
				return *((const px_word*) in) / (px_float)65535;
			case PX_GLTF_COMPONENT_TYPE_R_8:
				return *((const signed char*) in) / (px_float)127;
			case PX_GLTF_COMPONENT_TYPE_R_8U:
				return *((const px_byte*) in) / (px_float)255;
			default:
				return 0;
		}
	}

	return (px_float)PX_Gltf_ComponentReadInteger(in, component_type);
}

static px_bool PX_Gltf_ElementReadFloat(const px_byte* element, PX_GLTF_TYPE type, PX_GLTF_COMPONENT_TYPE component_type, px_bool normalized, px_float* out, px_gltf_size element_size)
{
	px_gltf_size num_components = PX_GltfNumComponents(type);

	if (element_size < num_components) {
		return 0;
	}

	// There are three special cases for component extraction, see #data-alignment in the 2.0 spec.

	px_gltf_size component_size = PX_GltfComponentSize(component_type);

	if (type == PX_GLTF_TYPE_MAT2 && component_size == 1)
	{
		out[0] = PX_Gltf_ComponentReadFloat(element, component_type, normalized);
		out[1] = PX_Gltf_ComponentReadFloat(element + 1, component_type, normalized);
		out[2] = PX_Gltf_ComponentReadFloat(element + 4, component_type, normalized);
		out[3] = PX_Gltf_ComponentReadFloat(element + 5, component_type, normalized);
		return 1;
	}

	if (type == PX_GLTF_TYPE_MAT3 && component_size == 1)
	{
		out[0] = PX_Gltf_ComponentReadFloat(element, component_type, normalized);
		out[1] = PX_Gltf_ComponentReadFloat(element + 1, component_type, normalized);
		out[2] = PX_Gltf_ComponentReadFloat(element + 2, component_type, normalized);
		out[3] = PX_Gltf_ComponentReadFloat(element + 4, component_type, normalized);
		out[4] = PX_Gltf_ComponentReadFloat(element + 5, component_type, normalized);
		out[5] = PX_Gltf_ComponentReadFloat(element + 6, component_type, normalized);
		out[6] = PX_Gltf_ComponentReadFloat(element + 8, component_type, normalized);
		out[7] = PX_Gltf_ComponentReadFloat(element + 9, component_type, normalized);
		out[8] = PX_Gltf_ComponentReadFloat(element + 10, component_type, normalized);
		return 1;
	}

	if (type == PX_GLTF_TYPE_MAT3 && component_size == 2)
	{
		out[0] = PX_Gltf_ComponentReadFloat(element, component_type, normalized);
		out[1] = PX_Gltf_ComponentReadFloat(element + 2, component_type, normalized);
		out[2] = PX_Gltf_ComponentReadFloat(element + 4, component_type, normalized);
		out[3] = PX_Gltf_ComponentReadFloat(element + 8, component_type, normalized);
		out[4] = PX_Gltf_ComponentReadFloat(element + 10, component_type, normalized);
		out[5] = PX_Gltf_ComponentReadFloat(element + 12, component_type, normalized);
		out[6] = PX_Gltf_ComponentReadFloat(element + 16, component_type, normalized);
		out[7] = PX_Gltf_ComponentReadFloat(element + 18, component_type, normalized);
		out[8] = PX_Gltf_ComponentReadFloat(element + 20, component_type, normalized);
		return 1;
	}

	for (px_gltf_size i = 0; i < num_components; ++i)
	{
		out[i] = PX_Gltf_ComponentReadFloat(element + component_size * i, component_type, normalized);
	}
	return 1;
}

const px_byte* PX_GltfBufferViewData(const PX_Gltf_BufferView* view)
{
	if (view->data)
		return (const px_byte*)view->data;

	if (!view->buffer->data)
		return PX_NULL;

	const px_byte* result = (const px_byte*)view->buffer->data;
	result += view->offset;
	return result;
}

const PX_Gltf_Accessor* PX_GltfFindAccessor(const PX_Gltf_Primitive* prim, PX_GLTF_ATTRIBUTE_TYPE type, px_int index)
{
	for (px_gltf_size i = 0; i < prim->attributes_count; ++i)
	{
		const PX_Gltf_Attribute* attr = &prim->attributes[i];
		if (attr->type == type && attr->index == index)
			return attr->data;
	}

	return PX_NULL;
}

static const px_byte* PX_Gltf_FindSparseIndex(const PX_Gltf_Accessor* accessor, px_gltf_size needle)
{
	const PX_Gltf_AccessorSparse* sparse = &accessor->sparse;
	const px_byte* index_data = PX_GltfBufferViewData(sparse->indices_buffer_view);
	const px_byte* value_data = PX_GltfBufferViewData(sparse->values_buffer_view);

	if (index_data == PX_NULL || value_data == PX_NULL)
		return PX_NULL;

	index_data += sparse->indices_byte_offset;
	value_data += sparse->values_byte_offset;

	px_gltf_size index_stride = PX_GltfComponentSize(sparse->indices_component_type);

	px_gltf_size offset = 0;
	px_gltf_size length = sparse->count;

	while (length)
	{
		px_gltf_size rem = length % 2;
		length /= 2;

		px_gltf_size index = PX_Gltf_ComponentReadIndex(index_data + (offset + length) * index_stride, sparse->indices_component_type);
		offset += index < needle ? length + rem : 0;
	}

	if (offset == sparse->count)
		return PX_NULL;

	px_gltf_size index = PX_Gltf_ComponentReadIndex(index_data + offset * index_stride, sparse->indices_component_type);
	return index == needle ? value_data + offset * accessor->stride : PX_NULL;
}

px_bool PX_GltfAccessorReadFloat(const PX_Gltf_Accessor* accessor, px_gltf_size index, px_float* out, px_gltf_size element_size)
{
	if (accessor->is_sparse)
	{
		const px_byte* element = PX_Gltf_FindSparseIndex(accessor, index);
		if (element)
			return PX_Gltf_ElementReadFloat(element, accessor->type, accessor->component_type, accessor->normalized, out, element_size);
	}
	if (accessor->buffer_view == PX_NULL)
	{
		PX_memset(out, 0, (px_int)(element_size * sizeof(px_float)));
		return 1;
	}
	const px_byte* element = PX_GltfBufferViewData(accessor->buffer_view);
	if (element == PX_NULL)
	{
		return 0;
	}
	element += accessor->offset + accessor->stride * index;
	return PX_Gltf_ElementReadFloat(element, accessor->type, accessor->component_type, accessor->normalized, out, element_size);
}

px_gltf_size PX_GltfAccessorUnpackFloats(const PX_Gltf_Accessor* accessor, px_float* out, px_gltf_size float_count)
{
	px_gltf_size floats_per_element = PX_GltfNumComponents(accessor->type);
	px_gltf_size available_floats = accessor->count * floats_per_element;
	if (out == PX_NULL)
	{
		return available_floats;
	}

	float_count = available_floats < float_count ? available_floats : float_count;
	px_gltf_size element_count = float_count / floats_per_element;

	// First pass: convert each element in the base accessor.
	if (accessor->buffer_view == PX_NULL)
	{
		PX_memset(out, 0, (px_int)(element_count * floats_per_element * sizeof(px_float)));
	}
	else
	{
		const px_byte* element = PX_GltfBufferViewData(accessor->buffer_view);
		if (element == PX_NULL)
		{
			return 0;
		}
		element += accessor->offset;

		if (accessor->component_type == PX_GLTF_COMPONENT_TYPE_R_32F && accessor->stride == floats_per_element * sizeof(px_float))
		{
			PX_memcpy(out, element, (px_int)(element_count * floats_per_element * sizeof(px_float)));
		}
		else
		{
			px_float* dest = out;

			for (px_gltf_size index = 0; index < element_count; index++, dest += floats_per_element, element += accessor->stride)
			{
				if (!PX_Gltf_ElementReadFloat(element, accessor->type, accessor->component_type, accessor->normalized, dest, floats_per_element))
				{
					return 0;
				}
			}
		}
	}

	// Second pass: write out each element in the sparse accessor.
	if (accessor->is_sparse)
	{
		const PX_Gltf_AccessorSparse* sparse = &accessor->sparse;

		const px_byte* index_data = PX_GltfBufferViewData(sparse->indices_buffer_view);
		const px_byte* reader_head = PX_GltfBufferViewData(sparse->values_buffer_view);

		if (index_data == PX_NULL || reader_head == PX_NULL)
		{
			return 0;
		}

		index_data += sparse->indices_byte_offset;
		reader_head += sparse->values_byte_offset;

		px_gltf_size index_stride = PX_GltfComponentSize(sparse->indices_component_type);
		for (px_gltf_size reader_index = 0; reader_index < sparse->count; reader_index++, index_data += index_stride, reader_head += accessor->stride)
		{
			px_gltf_size writer_index = PX_Gltf_ComponentReadIndex(index_data, sparse->indices_component_type);
			px_float* writer_head = out + writer_index * floats_per_element;

			if (!PX_Gltf_ElementReadFloat(reader_head, accessor->type, accessor->component_type, accessor->normalized, writer_head, floats_per_element))
			{
				return 0;
			}
		}
	}

	return element_count * floats_per_element;
}

static px_uint PX_Gltf_ComponentReadUint(const px_void* in, PX_GLTF_COMPONENT_TYPE component_type)
{
	switch (component_type)
	{
		case PX_GLTF_COMPONENT_TYPE_R_8:
			return *((const signed char*) in);

		case PX_GLTF_COMPONENT_TYPE_R_8U:
			return *((const px_byte*) in);

		case PX_GLTF_COMPONENT_TYPE_R_16:
			return *((const px_short*) in);

		case PX_GLTF_COMPONENT_TYPE_R_16U:
			return *((const px_word*) in);

		case PX_GLTF_COMPONENT_TYPE_R_32U:
			return *((const px_dword*) in);

		default:
			return 0;
	}
}

static px_bool PX_Gltf_ElementReadUint(const px_byte* element, PX_GLTF_TYPE type, PX_GLTF_COMPONENT_TYPE component_type, px_uint* out, px_gltf_size element_size)
{
	px_gltf_size num_components = PX_GltfNumComponents(type);

	if (element_size < num_components)
	{
		return 0;
	}

	// Reading integer matrices is not a valid use case
	if (type == PX_GLTF_TYPE_MAT2 || type == PX_GLTF_TYPE_MAT3 || type == PX_GLTF_TYPE_MAT4)
	{
		return 0;
	}

	px_gltf_size component_size = PX_GltfComponentSize(component_type);

	for (px_gltf_size i = 0; i < num_components; ++i)
	{
		out[i] = PX_Gltf_ComponentReadUint(element + component_size * i, component_type);
	}
	return 1;
}

px_bool PX_GltfAccessorReadUint(const PX_Gltf_Accessor* accessor, px_gltf_size index, px_uint* out, px_gltf_size element_size)
{
	if (accessor->is_sparse)
	{
		const px_byte* element = PX_Gltf_FindSparseIndex(accessor, index);
		if (element)
			return PX_Gltf_ElementReadUint(element, accessor->type, accessor->component_type, out, element_size);
	}
	if (accessor->buffer_view == PX_NULL)
	{
		PX_memset(out, 0, (px_int)(element_size * sizeof(px_uint)));
		return 1;
	}
	const px_byte* element = PX_GltfBufferViewData(accessor->buffer_view);
	if (element == PX_NULL)
	{
		return 0;
	}
	element += accessor->offset + accessor->stride * index;
	return PX_Gltf_ElementReadUint(element, accessor->type, accessor->component_type, out, element_size);
}

px_gltf_size PX_GltfAccessorReadIndex(const PX_Gltf_Accessor* accessor, px_gltf_size index)
{
	if (accessor->is_sparse)
	{
		const px_byte* element = PX_Gltf_FindSparseIndex(accessor, index);
		if (element)
			return PX_Gltf_ComponentReadIndex(element, accessor->component_type);
	}
	if (accessor->buffer_view == PX_NULL)
	{
		return 0;
	}
	const px_byte* element = PX_GltfBufferViewData(accessor->buffer_view);
	if (element == PX_NULL)
	{
		return 0; // This is an error case, but we can't communicate the error with existing interface.
	}
	element += accessor->offset + accessor->stride * index;
	return PX_Gltf_ComponentReadIndex(element, accessor->component_type);
}

px_gltf_size PX_GltfMeshIndex(const PX_Gltf_Data* data, const PX_Gltf_Mesh* object)
{
	PX_ASSERTIF(!(object && (px_gltf_size)(object - data->meshes) < data->meshes_count));
	return (px_gltf_size)(object - data->meshes);
}

px_gltf_size PX_GltfMaterialIndex(const PX_Gltf_Data* data, const PX_Gltf_Material* object)
{
	PX_ASSERTIF(!(object && (px_gltf_size)(object - data->materials) < data->materials_count));
	return (px_gltf_size)(object - data->materials);
}

px_gltf_size PX_GltfAccessorIndex(const PX_Gltf_Data* data, const PX_Gltf_Accessor* object)
{
	PX_ASSERTIF(!(object && (px_gltf_size)(object - data->accessors) < data->accessors_count));
	return (px_gltf_size)(object - data->accessors);
}

px_gltf_size PX_GltfBufferViewIndex(const PX_Gltf_Data* data, const PX_Gltf_BufferView* object)
{
	PX_ASSERTIF(!(object && (px_gltf_size)(object - data->buffer_views) < data->buffer_views_count));
	return (px_gltf_size)(object - data->buffer_views);
}

px_gltf_size PX_GltfBufferIndex(const PX_Gltf_Data* data, const PX_Gltf_Buffer* object)
{
	PX_ASSERTIF(!(object && (px_gltf_size)(object - data->buffers) < data->buffers_count));
	return (px_gltf_size)(object - data->buffers);
}

px_gltf_size PX_GltfImageIndex(const PX_Gltf_Data* data, const PX_Gltf_Image* object)
{
	PX_ASSERTIF(!(object && (px_gltf_size)(object - data->images) < data->images_count));
	return (px_gltf_size)(object - data->images);
}

px_gltf_size PX_GltfTextureIndex(const PX_Gltf_Data* data, const PX_Gltf_Texture* object)
{
	PX_ASSERTIF(!(object && (px_gltf_size)(object - data->textures) < data->textures_count));
	return (px_gltf_size)(object - data->textures);
}

px_gltf_size PX_GltfSamplerIndex(const PX_Gltf_Data* data, const PX_Gltf_Sampler* object)
{
	PX_ASSERTIF(!(object && (px_gltf_size)(object - data->samplers) < data->samplers_count));
	return (px_gltf_size)(object - data->samplers);
}

px_gltf_size PX_GltfSkinIndex(const PX_Gltf_Data* data, const PX_Gltf_Skin* object)
{
	PX_ASSERTIF(!(object && (px_gltf_size)(object - data->skins) < data->skins_count));
	return (px_gltf_size)(object - data->skins);
}

px_gltf_size PX_GltfCameraIndex(const PX_Gltf_Data* data, const PX_Gltf_Camera* object)
{
	PX_ASSERTIF(!(object && (px_gltf_size)(object - data->cameras) < data->cameras_count));
	return (px_gltf_size)(object - data->cameras);
}

px_gltf_size PX_GltfLightIndex(const PX_Gltf_Data* data, const PX_Gltf_Light* object)
{
	PX_ASSERTIF(!(object && (px_gltf_size)(object - data->lights) < data->lights_count));
	return (px_gltf_size)(object - data->lights);
}

px_gltf_size PX_GltfNodeIndex(const PX_Gltf_Data* data, const PX_Gltf_Node* object)
{
	PX_ASSERTIF(!(object && (px_gltf_size)(object - data->nodes) < data->nodes_count));
	return (px_gltf_size)(object - data->nodes);
}

px_gltf_size PX_GltfSceneIndex(const PX_Gltf_Data* data, const PX_Gltf_Scene* object)
{
	PX_ASSERTIF(!(object && (px_gltf_size)(object - data->scenes) < data->scenes_count));
	return (px_gltf_size)(object - data->scenes);
}

px_gltf_size PX_GltfAnimationIndex(const PX_Gltf_Data* data, const PX_Gltf_Animation* object)
{
	PX_ASSERTIF(!(object && (px_gltf_size)(object - data->animations) < data->animations_count));
	return (px_gltf_size)(object - data->animations);
}

px_gltf_size PX_GltfAnimationSamplerIndex(const PX_Gltf_Animation* animation, const PX_Gltf_AnimationSampler* object)
{
	PX_ASSERTIF(!(object && (px_gltf_size)(object - animation->samplers) < animation->samplers_count));
	return (px_gltf_size)(object - animation->samplers);
}

px_gltf_size PX_GltfAnimationChannelIndex(const PX_Gltf_Animation* animation, const PX_Gltf_AnimationChannel* object)
{
	PX_ASSERTIF(!(object && (px_gltf_size)(object - animation->channels) < animation->channels_count));
	return (px_gltf_size)(object - animation->channels);
}

px_gltf_size PX_GltfAccessorUnpackIndices(const PX_Gltf_Accessor* accessor, px_void* out, px_gltf_size out_component_size, px_gltf_size index_count)
{
	if (out == PX_NULL)
	{
		return accessor->count;
	}

	px_gltf_size numbers_per_element = PX_GltfNumComponents(accessor->type);
	px_gltf_size available_numbers = accessor->count * numbers_per_element;

	index_count = available_numbers < index_count ? available_numbers : index_count;
	px_gltf_size index_component_size = PX_GltfComponentSize(accessor->component_type);

	if (accessor->is_sparse)
	{
		return 0;
	}
	if (accessor->buffer_view == PX_NULL)
	{
		return 0;
	}
	if (index_component_size > out_component_size)
	{
		return 0;
	}
	const px_byte* element = PX_GltfBufferViewData(accessor->buffer_view);
	if (element == PX_NULL)
	{
		return 0;
	}
	element += accessor->offset;

	if (index_component_size == out_component_size && accessor->stride == out_component_size * numbers_per_element)
	{
		PX_memcpy(out, element, (px_int)(index_count * index_component_size));
		return index_count;
	}

	// Data couldn't be copied with memcpy due to stride being larger than the component size.
	// OR
	// The component size of the output array is larger than the component size of the index data, so index data will be padded.
	switch (out_component_size)
	{
	case 1:
		for (px_gltf_size index = 0; index < index_count; index++, element += accessor->stride)
		{
			((px_byte*)out)[index] = (px_byte)PX_Gltf_ComponentReadIndex(element, accessor->component_type);
		}
		break;
	case 2:
		for (px_gltf_size index = 0; index < index_count; index++, element += accessor->stride)
		{
			((px_word*)out)[index] = (px_word)PX_Gltf_ComponentReadIndex(element, accessor->component_type);
		}
		break;
	case 4:
		for (px_gltf_size index = 0; index < index_count; index++, element += accessor->stride)
		{
			((px_dword*)out)[index] = (px_dword)PX_Gltf_ComponentReadIndex(element, accessor->component_type);
		}
		break;
	default:
		return 0;
	}

	return index_count;
}

#define PX_GLTF_ERROR_JSON -1
#define PX_GLTF_ERROR_NOMEM -2
#define PX_GLTF_ERROR_LEGACY -3

#define PX_GLTF_CHECK_TOKTYPE(tok_, type_) if ((tok_).type != (type_)) { return PX_GLTF_ERROR_JSON; }
#define PX_GLTF_CHECK_TOKTYPE_RET(tok_, type_, ret_) if ((tok_).type != (type_)) { return ret_; }
#define PX_GLTF_CHECK_KEY(tok_) if ((tok_).type != PX_GLTF_JSMN_STRING || (tok_).size == 0) { return PX_GLTF_ERROR_JSON; } /* checking size for 0 verifies that a value follows the key */

#define PX_GLTF_PTRINDEX(type, idx) (type*)((px_gltf_size)idx + 1)
#define PX_GLTF_PTRFIXUP(var, data, size) if (var) { if ((px_gltf_size)var > size) { return PX_GLTF_ERROR_JSON; } var = &data[(px_gltf_size)var-1]; }
#define PX_GLTF_PTRFIXUP_REQ(var, data, size) if (!var || (px_gltf_size)var > size) { return PX_GLTF_ERROR_JSON; } var = &data[(px_gltf_size)var-1];

static px_int PX_Gltf_JsonStrcmp(PX_Gltf_JsmnToken const* tok, const px_byte* json_chunk, const px_char* str)
{
	PX_GLTF_CHECK_TOKTYPE(*tok, PX_GLTF_JSMN_STRING);
	px_gltf_size const str_len = PX_strlen(str);
	px_gltf_size const name_length = (px_gltf_size)(tok->end - tok->start);
	return (str_len == name_length) ? PX_Gltf_Strncmp((const px_char*)json_chunk + tok->start, str, str_len) : 128;
}

static px_int PX_Gltf_JsonToInt(PX_Gltf_JsmnToken const* tok, const px_byte* json_chunk)
{
	PX_GLTF_CHECK_TOKTYPE(*tok, PX_GLTF_JSMN_PRIMITIVE);
	return (px_int)PX_Gltf_StringToInt64((const px_char*)json_chunk + tok->start, (px_int)(tok->end - tok->start));
}

static px_gltf_size PX_Gltf_JsonToSize(PX_Gltf_JsmnToken const* tok, const px_byte* json_chunk)
{
	PX_GLTF_CHECK_TOKTYPE_RET(*tok, PX_GLTF_JSMN_PRIMITIVE, 0);
	px_int64 res = PX_Gltf_StringToInt64((const px_char*)json_chunk + tok->start, (px_int)(tok->end - tok->start));
	return res < 0 ? 0 : (px_gltf_size)res;
}

static px_float PX_Gltf_JsonToFloat(PX_Gltf_JsmnToken const* tok, const px_byte* json_chunk)
{
	PX_GLTF_CHECK_TOKTYPE(*tok, PX_GLTF_JSMN_PRIMITIVE);
	return (px_float)PX_Gltf_StringToDouble((const px_char*)json_chunk + tok->start, (px_int)(tok->end - tok->start));
}

static px_bool PX_Gltf_JsonToBool(PX_Gltf_JsmnToken const* tok, const px_byte* json_chunk)
{
	px_int size = (px_int)(tok->end - tok->start);
	return size == 4 && PX_memcmp((px_void*)(json_chunk + tok->start), "true", 4) == 0;
}

static px_int PX_Gltf_SkipJson(PX_Gltf_JsmnToken const* tokens, px_int i)
{
	px_int end = i + 1;

	while (i < end)
	{
		switch (tokens[i].type)
		{
		case PX_GLTF_JSMN_OBJECT:
			end += tokens[i].size * 2;
			break;

		case PX_GLTF_JSMN_ARRAY:
			end += tokens[i].size;
			break;

		case PX_GLTF_JSMN_PRIMITIVE:
		case PX_GLTF_JSMN_STRING:
			break;

		default:
			return -1;
		}

		i++;
	}

	return i;
}

static px_void PX_Gltf_FillFloatArray(px_float* out_array, px_int size, px_float value)
{
	for (px_int j = 0; j < size; ++j)
	{
		out_array[j] = value;
	}
}

static px_int PX_Gltf_ParseJsonFloatArray(PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, px_float* out_array, px_int size)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_ARRAY);
	if (tokens[i].size != size)
	{
		return PX_GLTF_ERROR_JSON;
	}
	++i;
	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_PRIMITIVE);
		out_array[j] = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
		++i;
	}
	return i;
}

static px_int PX_Gltf_ParseJsonString(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, px_char** out_string)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_STRING);
	if (*out_string)
	{
		return PX_GLTF_ERROR_JSON;
	}
	px_int size = (px_int)(tokens[i].end - tokens[i].start);
	px_char* result = (px_char*)options->memory.alloc_func(options->memory.user_data, size + 1);
	if (!result)
	{
		return PX_GLTF_ERROR_NOMEM;
	}
	PX_memcpy(result, (const px_char*)json_chunk + tokens[i].start, (px_int)(size));
	result[size] = 0;
	*out_string = result;
	return i + 1;
}

static px_int PX_Gltf_ParseJsonArray(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, px_gltf_size element_size, px_void** out_array, px_gltf_size* out_size)
{
	(px_void)json_chunk;
	if (tokens[i].type != PX_GLTF_JSMN_ARRAY)
	{
		return tokens[i].type == PX_GLTF_JSMN_OBJECT ? PX_GLTF_ERROR_LEGACY : PX_GLTF_ERROR_JSON;
	}
	if (*out_array)
	{
		return PX_GLTF_ERROR_JSON;
	}
	px_int size = tokens[i].size;
	px_void* result = PX_Gltf_Calloc(options, element_size, size);
	if (!result)
	{
		return PX_GLTF_ERROR_NOMEM;
	}
	*out_array = result;
	*out_size = size;
	return i + 1;
}

static px_int PX_Gltf_ParseJsonStringArray(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, px_char*** out_array, px_gltf_size* out_size)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_ARRAY);
	i = PX_Gltf_ParseJsonArray(options, tokens, i, json_chunk, sizeof(px_char*), (px_void**)out_array, out_size);
	if (i < 0)
	{
		return i;
	}

	for (px_gltf_size j = 0; j < *out_size; ++j)
	{
		i = PX_Gltf_ParseJsonString(options, tokens, i, json_chunk, j + (*out_array));
		if (i < 0)
		{
			return i;
		}
	}
	return i;
}

static px_void PX_Gltf_ParseAttributeType(const px_char* name, PX_GLTF_ATTRIBUTE_TYPE* out_type, px_int* out_index)
{
	if (*name == '_')
	{
		*out_type = PX_GLTF_ATTRIBUTE_TYPE_CUSTOM;
		return;
	}

	const px_char* us = PX_strchr(name, '_');
	px_gltf_size len = us ? (px_gltf_size)(us - name) : (px_gltf_size)PX_strlen(name);

	if (len == 8 && PX_Gltf_Strncmp(name, "POSITION", 8) == 0)
	{
		*out_type = PX_GLTF_ATTRIBUTE_TYPE_POSITION;
	}
	else if (len == 6 && PX_Gltf_Strncmp(name, "NORMAL", 6) == 0)
	{
		*out_type = PX_GLTF_ATTRIBUTE_TYPE_NORMAL;
	}
	else if (len == 7 && PX_Gltf_Strncmp(name, "TANGENT", 7) == 0)
	{
		*out_type = PX_GLTF_ATTRIBUTE_TYPE_TANGENT;
	}
	else if (len == 8 && PX_Gltf_Strncmp(name, "TEXCOORD", 8) == 0)
	{
		*out_type = PX_GLTF_ATTRIBUTE_TYPE_TEXCOORD;
	}
	else if (len == 5 && PX_Gltf_Strncmp(name, "COLOR", 5) == 0)
	{
		*out_type = PX_GLTF_ATTRIBUTE_TYPE_COLOR;
	}
	else if (len == 6 && PX_Gltf_Strncmp(name, "JOINTS", 6) == 0)
	{
		*out_type = PX_GLTF_ATTRIBUTE_TYPE_JOINTS;
	}
	else if (len == 7 && PX_Gltf_Strncmp(name, "WEIGHTS", 7) == 0)
	{
		*out_type = PX_GLTF_ATTRIBUTE_TYPE_WEIGHTS;
	}
	else
	{
		*out_type = PX_GLTF_ATTRIBUTE_TYPE_INVALID;
	}

	if (us && *out_type != PX_GLTF_ATTRIBUTE_TYPE_INVALID)
	{
		*out_index = (px_int)PX_Gltf_StringToInt64(us + 1, PX_strlen(us + 1));
		if (*out_index < 0)
		{
			*out_type = PX_GLTF_ATTRIBUTE_TYPE_INVALID;
			*out_index = 0;
		}
	}
}

static px_int PX_Gltf_ParseJsonAttributeList(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Attribute** out_attributes, px_gltf_size* out_attributes_count)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	if (*out_attributes)
	{
		return PX_GLTF_ERROR_JSON;
	}

	*out_attributes_count = tokens[i].size;
	*out_attributes = (PX_Gltf_Attribute*)PX_Gltf_Calloc(options, sizeof(PX_Gltf_Attribute), *out_attributes_count);
	++i;

	if (!*out_attributes)
	{
		return PX_GLTF_ERROR_NOMEM;
	}

	for (px_gltf_size j = 0; j < *out_attributes_count; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		i = PX_Gltf_ParseJsonString(options, tokens, i, json_chunk, &(*out_attributes)[j].name);
		if (i < 0)
		{
			return PX_GLTF_ERROR_JSON;
		}

		PX_Gltf_ParseAttributeType((*out_attributes)[j].name, &(*out_attributes)[j].type, &(*out_attributes)[j].index);

		(*out_attributes)[j].data = PX_GLTF_PTRINDEX(PX_Gltf_Accessor, PX_Gltf_JsonToInt(tokens + i, json_chunk));
		++i;
	}

	return i;
}

static px_int PX_Gltf_ParseJsonExtras(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Extras* out_extras)
{
	if (out_extras->data)
	{
		return PX_GLTF_ERROR_JSON;
	}

	/* fill deprecated fields for now, this will be removed in the future */
	out_extras->start_offset = tokens[i].start;
	out_extras->end_offset = tokens[i].end;

	px_gltf_size start = tokens[i].start;
	px_gltf_size size = tokens[i].end - start;
	out_extras->data = (px_char*)options->memory.alloc_func(options->memory.user_data, size + 1);
	if (!out_extras->data)
	{
		return PX_GLTF_ERROR_NOMEM;
	}
	PX_memcpy(out_extras->data, (const px_char*)json_chunk + start, (px_int)(size));
	out_extras->data[size] = '\0';

	i = PX_Gltf_SkipJson(tokens, i);
	return i;
}

static px_int PX_Gltf_ParseJsonUnprocessedExtension(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Extension* out_extension)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_STRING);
	PX_GLTF_CHECK_TOKTYPE(tokens[i+1], PX_GLTF_JSMN_OBJECT);
	if (out_extension->name)
	{
		return PX_GLTF_ERROR_JSON;
	}

	px_gltf_size name_length = tokens[i].end - tokens[i].start;
	out_extension->name = (px_char*)options->memory.alloc_func(options->memory.user_data, name_length + 1);
	if (!out_extension->name)
	{
		return PX_GLTF_ERROR_NOMEM;
	}
	PX_memcpy(out_extension->name, (const px_char*)json_chunk + tokens[i].start, (px_int)(name_length));
	out_extension->name[name_length] = 0;
	i++;

	px_gltf_size start = tokens[i].start;
	px_gltf_size size = tokens[i].end - start;
	out_extension->data = (px_char*)options->memory.alloc_func(options->memory.user_data, size + 1);
	if (!out_extension->data)
	{
		return PX_GLTF_ERROR_NOMEM;
	}
	PX_memcpy(out_extension->data, (const px_char*)json_chunk + start, (px_int)(size));
	out_extension->data[size] = '\0';

	i = PX_Gltf_SkipJson(tokens, i);

	return i;
}

static px_int PX_Gltf_ParseJsonUnprocessedExtensions(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, px_gltf_size* out_extensions_count, PX_Gltf_Extension** out_extensions)
{
	++i;

	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
	if(*out_extensions)
	{
		return PX_GLTF_ERROR_JSON;
	}

	px_int extensions_size = tokens[i].size;
	*out_extensions_count = 0;
	*out_extensions = (PX_Gltf_Extension*)PX_Gltf_Calloc(options, sizeof(PX_Gltf_Extension), extensions_size);

	if (!*out_extensions)
	{
		return PX_GLTF_ERROR_NOMEM;
	}

	++i;

	for (px_int j = 0; j < extensions_size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		px_gltf_size extension_index = (*out_extensions_count)++;
		PX_Gltf_Extension* extension = &((*out_extensions)[extension_index]);
		i = PX_Gltf_ParseJsonUnprocessedExtension(options, tokens, i, json_chunk, extension);

		if (i < 0)
		{
			return i;
		}
	}
	return i;
}

static px_int PX_Gltf_ParseJsonDracoMeshCompression(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_DracoMeshCompression* out_draco_mesh_compression)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "attributes") == 0)
		{
			i = PX_Gltf_ParseJsonAttributeList(options, tokens, i + 1, json_chunk, &out_draco_mesh_compression->attributes, &out_draco_mesh_compression->attributes_count);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "bufferView") == 0)
		{
			++i;
			out_draco_mesh_compression->buffer_view = PX_GLTF_PTRINDEX(PX_Gltf_BufferView, PX_Gltf_JsonToInt(tokens + i, json_chunk));
			++i;
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonMeshGpuInstancing(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_MeshGpuInstancing* out_mesh_gpu_instancing)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "attributes") == 0)
		{
			i = PX_Gltf_ParseJsonAttributeList(options, tokens, i + 1, json_chunk, &out_mesh_gpu_instancing->attributes, &out_mesh_gpu_instancing->attributes_count);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonMaterialMappingData(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_MaterialMapping* out_mappings, px_gltf_size* offset)
{
	(px_void)options;
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_ARRAY);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

		px_int obj_size = tokens[i].size;
		++i;

		px_int material = -1;
		px_int variants_tok = -1;
		px_int extras_tok = -1;

		for (px_int k = 0; k < obj_size; ++k)
		{
			PX_GLTF_CHECK_KEY(tokens[i]);

			if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "material") == 0)
			{
				++i;
				material = PX_Gltf_JsonToInt(tokens + i, json_chunk);
				++i;
			}
			else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "variants") == 0)
			{
				variants_tok = i+1;
				PX_GLTF_CHECK_TOKTYPE(tokens[variants_tok], PX_GLTF_JSMN_ARRAY);

				i = PX_Gltf_SkipJson(tokens, i+1);
			}
			else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
			{
				extras_tok = i + 1;
				i = PX_Gltf_SkipJson(tokens, extras_tok);
			}
			else
			{
				i = PX_Gltf_SkipJson(tokens, i+1);
			}

			if (i < 0)
			{
				return i;
			}
		}

		if (material < 0 || variants_tok < 0)
		{
			return PX_GLTF_ERROR_JSON;
		}

		if (out_mappings)
		{
			for (px_int k = 0; k < tokens[variants_tok].size; ++k)
			{
				px_int variant = PX_Gltf_JsonToInt(&tokens[variants_tok + 1 + k], json_chunk);
				if (variant < 0)
					return variant;

				out_mappings[*offset].material = PX_GLTF_PTRINDEX(PX_Gltf_Material, material);
				out_mappings[*offset].variant = variant;

				if (extras_tok >= 0)
				{
					px_int e = PX_Gltf_ParseJsonExtras(options, tokens, extras_tok, json_chunk, &out_mappings[*offset].extras);
					if (e < 0)
						return e;
				}

				(*offset)++;
			}
		}
		else
		{
			(*offset) += tokens[variants_tok].size;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonMaterialMappings(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Primitive* out_prim)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "mappings") == 0)
		{
			if (out_prim->mappings)
			{
				return PX_GLTF_ERROR_JSON;
			}

			px_gltf_size mappings_offset = 0;
			px_int k = PX_Gltf_ParseJsonMaterialMappingData(options, tokens, i + 1, json_chunk, PX_NULL, &mappings_offset);
			if (k < 0)
			{
				return k;
			}

			out_prim->mappings_count = mappings_offset;
			out_prim->mappings = (PX_Gltf_MaterialMapping*)PX_Gltf_Calloc(options, sizeof(PX_Gltf_MaterialMapping), out_prim->mappings_count);

			mappings_offset = 0;
			i = PX_Gltf_ParseJsonMaterialMappingData(options, tokens, i + 1, json_chunk, out_prim->mappings, &mappings_offset);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static PX_GLTF_PRIMITIVE_TYPE PX_Gltf_JsonToPrimitiveType(PX_Gltf_JsmnToken const* tok, const px_byte* json_chunk)
{
	px_int type = PX_Gltf_JsonToInt(tok, json_chunk);

	switch (type)
	{
	case 0:
		return PX_GLTF_PRIMITIVE_TYPE_POINTS;
	case 1:
		return PX_GLTF_PRIMITIVE_TYPE_LINES;
	case 2:
		return PX_GLTF_PRIMITIVE_TYPE_LINE_LOOP;
	case 3:
		return PX_GLTF_PRIMITIVE_TYPE_LINE_STRIP;
	case 4:
		return PX_GLTF_PRIMITIVE_TYPE_TRIANGLES;
	case 5:
		return PX_GLTF_PRIMITIVE_TYPE_TRIANGLE_STRIP;
	case 6:
		return PX_GLTF_PRIMITIVE_TYPE_TRIANGLE_FAN;
	default:
		return PX_GLTF_PRIMITIVE_TYPE_INVALID;
	}
}

static px_int PX_Gltf_ParseJsonPrimitive(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Primitive* out_prim)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	out_prim->type = PX_GLTF_PRIMITIVE_TYPE_TRIANGLES;

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "mode") == 0)
		{
			++i;
			out_prim->type = PX_Gltf_JsonToPrimitiveType(tokens+i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "indices") == 0)
		{
			++i;
			out_prim->indices = PX_GLTF_PTRINDEX(PX_Gltf_Accessor, PX_Gltf_JsonToInt(tokens + i, json_chunk));
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "material") == 0)
		{
			++i;
			out_prim->material = PX_GLTF_PTRINDEX(PX_Gltf_Material, PX_Gltf_JsonToInt(tokens + i, json_chunk));
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "attributes") == 0)
		{
			i = PX_Gltf_ParseJsonAttributeList(options, tokens, i + 1, json_chunk, &out_prim->attributes, &out_prim->attributes_count);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "targets") == 0)
		{
			i = PX_Gltf_ParseJsonArray(options, tokens, i + 1, json_chunk, sizeof(PX_Gltf_MorphTarget), (px_void**)&out_prim->targets, &out_prim->targets_count);
			if (i < 0)
			{
				return i;
			}

			for (px_gltf_size k = 0; k < out_prim->targets_count; ++k)
			{
				i = PX_Gltf_ParseJsonAttributeList(options, tokens, i, json_chunk, &out_prim->targets[k].attributes, &out_prim->targets[k].attributes_count);
				if (i < 0)
				{
					return i;
				}
			}
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
		{
			i = PX_Gltf_ParseJsonExtras(options, tokens, i + 1, json_chunk, &out_prim->extras);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extensions") == 0)
		{
			++i;

			PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
			if(out_prim->extensions)
			{
				return PX_GLTF_ERROR_JSON;
			}

			px_int extensions_size = tokens[i].size;
			out_prim->extensions_count = 0;
			out_prim->extensions = (PX_Gltf_Extension*)PX_Gltf_Calloc(options, sizeof(PX_Gltf_Extension), extensions_size);

			if (!out_prim->extensions)
			{
				return PX_GLTF_ERROR_NOMEM;
			}

			++i;
			for (px_int k = 0; k < extensions_size; ++k)
			{
				PX_GLTF_CHECK_KEY(tokens[i]);

				if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "KHR_draco_mesh_compression") == 0)
				{
					out_prim->has_draco_mesh_compression = 1;
					i = PX_Gltf_ParseJsonDracoMeshCompression(options, tokens, i + 1, json_chunk, &out_prim->draco_mesh_compression);
				}
				else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "KHR_materials_variants") == 0)
				{
					i = PX_Gltf_ParseJsonMaterialMappings(options, tokens, i + 1, json_chunk, out_prim);
				}
				else
				{
					i = PX_Gltf_ParseJsonUnprocessedExtension(options, tokens, i, json_chunk, &(out_prim->extensions[out_prim->extensions_count++]));
				}

				if (i < 0)
				{
					return i;
				}
			}
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonMesh(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Mesh* out_mesh)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "name") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_mesh->name);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "primitives") == 0)
		{
			i = PX_Gltf_ParseJsonArray(options, tokens, i + 1, json_chunk, sizeof(PX_Gltf_Primitive), (px_void**)&out_mesh->primitives, &out_mesh->primitives_count);
			if (i < 0)
			{
				return i;
			}

			for (px_gltf_size prim_index = 0; prim_index < out_mesh->primitives_count; ++prim_index)
			{
				i = PX_Gltf_ParseJsonPrimitive(options, tokens, i, json_chunk, &out_mesh->primitives[prim_index]);
				if (i < 0)
				{
					return i;
				}
			}
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "weights") == 0)
		{
			i = PX_Gltf_ParseJsonArray(options, tokens, i + 1, json_chunk, sizeof(px_float), (px_void**)&out_mesh->weights, &out_mesh->weights_count);
			if (i < 0)
			{
				return i;
			}

			i = PX_Gltf_ParseJsonFloatArray(tokens, i - 1, json_chunk, out_mesh->weights, (px_int)out_mesh->weights_count);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
		{
			++i;

			out_mesh->extras.start_offset = tokens[i].start;
			out_mesh->extras.end_offset = tokens[i].end;

			if (tokens[i].type == PX_GLTF_JSMN_OBJECT)
			{
				px_int extras_size = tokens[i].size;
				++i;

				for (px_int k = 0; k < extras_size; ++k)
				{
					PX_GLTF_CHECK_KEY(tokens[i]);

					if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "targetNames") == 0 && tokens[i+1].type == PX_GLTF_JSMN_ARRAY)
					{
						i = PX_Gltf_ParseJsonStringArray(options, tokens, i + 1, json_chunk, &out_mesh->target_names, &out_mesh->target_names_count);
					}
					else
					{
						i = PX_Gltf_SkipJson(tokens, i+1);
					}

					if (i < 0)
					{
						return i;
					}
				}
			}
			else
			{
				i = PX_Gltf_SkipJson(tokens, i);
			}
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extensions") == 0)
		{
			i = PX_Gltf_ParseJsonUnprocessedExtensions(options, tokens, i, json_chunk, &out_mesh->extensions_count, &out_mesh->extensions);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonMeshes(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Data* out_data)
{
	i = PX_Gltf_ParseJsonArray(options, tokens, i, json_chunk, sizeof(PX_Gltf_Mesh), (px_void**)&out_data->meshes, &out_data->meshes_count);
	if (i < 0)
	{
		return i;
	}

	for (px_gltf_size j = 0; j < out_data->meshes_count; ++j)
	{
		i = PX_Gltf_ParseJsonMesh(options, tokens, i, json_chunk, &out_data->meshes[j]);
		if (i < 0)
		{
			return i;
		}
	}
	return i;
}

static PX_GLTF_COMPONENT_TYPE PX_Gltf_JsonToComponentType(PX_Gltf_JsmnToken const* tok, const px_byte* json_chunk)
{
	px_int type = PX_Gltf_JsonToInt(tok, json_chunk);

	switch (type)
	{
	case 5120:
		return PX_GLTF_COMPONENT_TYPE_R_8;
	case 5121:
		return PX_GLTF_COMPONENT_TYPE_R_8U;
	case 5122:
		return PX_GLTF_COMPONENT_TYPE_R_16;
	case 5123:
		return PX_GLTF_COMPONENT_TYPE_R_16U;
	case 5125:
		return PX_GLTF_COMPONENT_TYPE_R_32U;
	case 5126:
		return PX_GLTF_COMPONENT_TYPE_R_32F;
	default:
		return PX_GLTF_COMPONENT_TYPE_INVALID;
	}
}

static px_int PX_Gltf_ParseJsonAccessorSparse(PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_AccessorSparse* out_sparse)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "count") == 0)
		{
			++i;
			out_sparse->count = PX_Gltf_JsonToSize(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "indices") == 0)
		{
			++i;
			PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

			px_int indices_size = tokens[i].size;
			++i;

			for (px_int k = 0; k < indices_size; ++k)
			{
				PX_GLTF_CHECK_KEY(tokens[i]);

				if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "bufferView") == 0)
				{
					++i;
					out_sparse->indices_buffer_view = PX_GLTF_PTRINDEX(PX_Gltf_BufferView, PX_Gltf_JsonToInt(tokens + i, json_chunk));
					++i;
				}
				else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "byteOffset") == 0)
				{
					++i;
					out_sparse->indices_byte_offset = PX_Gltf_JsonToSize(tokens + i, json_chunk);
					++i;
				}
				else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "componentType") == 0)
				{
					++i;
					out_sparse->indices_component_type = PX_Gltf_JsonToComponentType(tokens + i, json_chunk);
					++i;
				}
				else
				{
					i = PX_Gltf_SkipJson(tokens, i+1);
				}

				if (i < 0)
				{
					return i;
				}
			}
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "values") == 0)
		{
			++i;
			PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

			px_int values_size = tokens[i].size;
			++i;

			for (px_int k = 0; k < values_size; ++k)
			{
				PX_GLTF_CHECK_KEY(tokens[i]);

				if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "bufferView") == 0)
				{
					++i;
					out_sparse->values_buffer_view = PX_GLTF_PTRINDEX(PX_Gltf_BufferView, PX_Gltf_JsonToInt(tokens + i, json_chunk));
					++i;
				}
				else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "byteOffset") == 0)
				{
					++i;
					out_sparse->values_byte_offset = PX_Gltf_JsonToSize(tokens + i, json_chunk);
					++i;
				}
				else
				{
					i = PX_Gltf_SkipJson(tokens, i+1);
				}

				if (i < 0)
				{
					return i;
				}
			}
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonAccessor(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Accessor* out_accessor)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "name") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_accessor->name);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "bufferView") == 0)
		{
			++i;
			out_accessor->buffer_view = PX_GLTF_PTRINDEX(PX_Gltf_BufferView, PX_Gltf_JsonToInt(tokens + i, json_chunk));
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "byteOffset") == 0)
		{
			++i;
			out_accessor->offset =
					PX_Gltf_JsonToSize(tokens+i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "componentType") == 0)
		{
			++i;
			out_accessor->component_type = PX_Gltf_JsonToComponentType(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "normalized") == 0)
		{
			++i;
			out_accessor->normalized = PX_Gltf_JsonToBool(tokens+i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "count") == 0)
		{
			++i;
			out_accessor->count = PX_Gltf_JsonToSize(tokens+i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "type") == 0)
		{
			++i;
			if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "SCALAR") == 0)
			{
				out_accessor->type = PX_GLTF_TYPE_SCALAR;
			}
			else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "VEC2") == 0)
			{
				out_accessor->type = PX_GLTF_TYPE_VEC2;
			}
			else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "VEC3") == 0)
			{
				out_accessor->type = PX_GLTF_TYPE_VEC3;
			}
			else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "VEC4") == 0)
			{
				out_accessor->type = PX_GLTF_TYPE_VEC4;
			}
			else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "MAT2") == 0)
			{
				out_accessor->type = PX_GLTF_TYPE_MAT2;
			}
			else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "MAT3") == 0)
			{
				out_accessor->type = PX_GLTF_TYPE_MAT3;
			}
			else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "MAT4") == 0)
			{
				out_accessor->type = PX_GLTF_TYPE_MAT4;
			}
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "min") == 0)
		{
			++i;
			out_accessor->has_min = 1;
			// note: we can't parse the precise number of elements since type may not have been computed yet
			px_int min_size = tokens[i].size > 16 ? 16 : tokens[i].size;
			i = PX_Gltf_ParseJsonFloatArray(tokens, i, json_chunk, out_accessor->min, min_size);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "max") == 0)
		{
			++i;
			out_accessor->has_max = 1;
			// note: we can't parse the precise number of elements since type may not have been computed yet
			px_int max_size = tokens[i].size > 16 ? 16 : tokens[i].size;
			i = PX_Gltf_ParseJsonFloatArray(tokens, i, json_chunk, out_accessor->max, max_size);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "sparse") == 0)
		{
			out_accessor->is_sparse = 1;
			i = PX_Gltf_ParseJsonAccessorSparse(tokens, i + 1, json_chunk, &out_accessor->sparse);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
		{
			i = PX_Gltf_ParseJsonExtras(options, tokens, i + 1, json_chunk, &out_accessor->extras);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extensions") == 0)
		{
			i = PX_Gltf_ParseJsonUnprocessedExtensions(options, tokens, i, json_chunk, &out_accessor->extensions_count, &out_accessor->extensions);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonTextureTransform(PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_TextureTransform* out_texture_transform)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "offset") == 0)
		{
			i = PX_Gltf_ParseJsonFloatArray(tokens, i + 1, json_chunk, out_texture_transform->offset, 2);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "rotation") == 0)
		{
			++i;
			out_texture_transform->rotation = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "scale") == 0)
		{
			i = PX_Gltf_ParseJsonFloatArray(tokens, i + 1, json_chunk, out_texture_transform->scale, 2);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "texCoord") == 0)
		{
			++i;
			out_texture_transform->has_texcoord = 1;
			out_texture_transform->texcoord = PX_Gltf_JsonToInt(tokens + i, json_chunk);
			++i;
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i + 1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonTextureView(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_TextureView* out_texture_view)
{
	(px_void)options;

	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	out_texture_view->scale = 1.0f;
	PX_Gltf_FillFloatArray(out_texture_view->transform.scale, 2, 1.0f);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "index") == 0)
		{
			++i;
			out_texture_view->texture = PX_GLTF_PTRINDEX(PX_Gltf_Texture, PX_Gltf_JsonToInt(tokens + i, json_chunk));
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "texCoord") == 0)
		{
			++i;
			out_texture_view->texcoord = PX_Gltf_JsonToInt(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "scale") == 0)
		{
			++i;
			out_texture_view->scale = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "strength") == 0)
		{
			++i;
			out_texture_view->scale = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extensions") == 0)
		{
			++i;

			PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
			px_int extensions_size = tokens[i].size;

			++i;

			for (px_int k = 0; k < extensions_size; ++k)
			{
				PX_GLTF_CHECK_KEY(tokens[i]);

				if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "KHR_texture_transform") == 0)
				{
					out_texture_view->has_transform = 1;
					i = PX_Gltf_ParseJsonTextureTransform(tokens, i + 1, json_chunk, &out_texture_view->transform);
				}
				else
				{
					i = PX_Gltf_SkipJson(tokens, i + 1);
				}

				if (i < 0)
				{
					return i;
				}
			}
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i + 1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonPbrMetallicRoughness(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_PbrMetallicRoughness* out_pbr)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "metallicFactor") == 0)
		{
			++i;
			out_pbr->metallic_factor =
				PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "roughnessFactor") == 0)
		{
			++i;
			out_pbr->roughness_factor =
				PX_Gltf_JsonToFloat(tokens+i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "baseColorFactor") == 0)
		{
			i = PX_Gltf_ParseJsonFloatArray(tokens, i + 1, json_chunk, out_pbr->base_color_factor, 4);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "baseColorTexture") == 0)
		{
			i = PX_Gltf_ParseJsonTextureView(options, tokens, i + 1, json_chunk, &out_pbr->base_color_texture);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "metallicRoughnessTexture") == 0)
		{
			i = PX_Gltf_ParseJsonTextureView(options, tokens, i + 1, json_chunk, &out_pbr->metallic_roughness_texture);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonPbrSpecularGlossiness(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_PbrSpecularGlossiness* out_pbr)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "diffuseFactor") == 0)
		{
			i = PX_Gltf_ParseJsonFloatArray(tokens, i + 1, json_chunk, out_pbr->diffuse_factor, 4);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "specularFactor") == 0)
		{
			i = PX_Gltf_ParseJsonFloatArray(tokens, i + 1, json_chunk, out_pbr->specular_factor, 3);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "glossinessFactor") == 0)
		{
			++i;
			out_pbr->glossiness_factor = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "diffuseTexture") == 0)
		{
			i = PX_Gltf_ParseJsonTextureView(options, tokens, i + 1, json_chunk, &out_pbr->diffuse_texture);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "specularGlossinessTexture") == 0)
		{
			i = PX_Gltf_ParseJsonTextureView(options, tokens, i + 1, json_chunk, &out_pbr->specular_glossiness_texture);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonClearcoat(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Clearcoat* out_clearcoat)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "clearcoatFactor") == 0)
		{
			++i;
			out_clearcoat->clearcoat_factor = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "clearcoatRoughnessFactor") == 0)
		{
			++i;
			out_clearcoat->clearcoat_roughness_factor = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "clearcoatTexture") == 0)
		{
			i = PX_Gltf_ParseJsonTextureView(options, tokens, i + 1, json_chunk, &out_clearcoat->clearcoat_texture);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "clearcoatRoughnessTexture") == 0)
		{
			i = PX_Gltf_ParseJsonTextureView(options, tokens, i + 1, json_chunk, &out_clearcoat->clearcoat_roughness_texture);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "clearcoatNormalTexture") == 0)
		{
			i = PX_Gltf_ParseJsonTextureView(options, tokens, i + 1, json_chunk, &out_clearcoat->clearcoat_normal_texture);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonIor(PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Ior* out_ior)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
	px_int size = tokens[i].size;
	++i;

	// Default values
	out_ior->ior = 1.5f;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "ior") == 0)
		{
			++i;
			out_ior->ior = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonSpecular(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Specular* out_specular)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
	px_int size = tokens[i].size;
	++i;

	// Default values
	out_specular->specular_factor = 1.0f;
	PX_Gltf_FillFloatArray(out_specular->specular_color_factor, 3, 1.0f);

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "specularFactor") == 0)
		{
			++i;
			out_specular->specular_factor = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "specularColorFactor") == 0)
		{
			i = PX_Gltf_ParseJsonFloatArray(tokens, i + 1, json_chunk, out_specular->specular_color_factor, 3);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "specularTexture") == 0)
		{
			i = PX_Gltf_ParseJsonTextureView(options, tokens, i + 1, json_chunk, &out_specular->specular_texture);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "specularColorTexture") == 0)
		{
			i = PX_Gltf_ParseJsonTextureView(options, tokens, i + 1, json_chunk, &out_specular->specular_color_texture);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonTransmission(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Transmission* out_transmission)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "transmissionFactor") == 0)
		{
			++i;
			out_transmission->transmission_factor = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "transmissionTexture") == 0)
		{
			i = PX_Gltf_ParseJsonTextureView(options, tokens, i + 1, json_chunk, &out_transmission->transmission_texture);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonVolume(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Volume* out_volume)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "thicknessFactor") == 0)
		{
			++i;
			out_volume->thickness_factor = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "thicknessTexture") == 0)
		{
			i = PX_Gltf_ParseJsonTextureView(options, tokens, i + 1, json_chunk, &out_volume->thickness_texture);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "attenuationColor") == 0)
		{
			i = PX_Gltf_ParseJsonFloatArray(tokens, i + 1, json_chunk, out_volume->attenuation_color, 3);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "attenuationDistance") == 0)
		{
			++i;
			out_volume->attenuation_distance = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i + 1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonSheen(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Sheen* out_sheen)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "sheenColorFactor") == 0)
		{
			i = PX_Gltf_ParseJsonFloatArray(tokens, i + 1, json_chunk, out_sheen->sheen_color_factor, 3);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "sheenColorTexture") == 0)
		{
			i = PX_Gltf_ParseJsonTextureView(options, tokens, i + 1, json_chunk, &out_sheen->sheen_color_texture);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "sheenRoughnessFactor") == 0)
		{
			++i;
			out_sheen->sheen_roughness_factor = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "sheenRoughnessTexture") == 0)
		{
			i = PX_Gltf_ParseJsonTextureView(options, tokens, i + 1, json_chunk, &out_sheen->sheen_roughness_texture);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonEmissiveStrength(PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_EmissiveStrength* out_emissive_strength)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
	px_int size = tokens[i].size;
	++i;

	// Default
	out_emissive_strength->emissive_strength = 1.f;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "emissiveStrength") == 0)
		{
			++i;
			out_emissive_strength->emissive_strength = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i + 1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonIridescence(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Iridescence* out_iridescence)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
	px_int size = tokens[i].size;
	++i;

	// Default
	out_iridescence->iridescence_ior = 1.3f;
	out_iridescence->iridescence_thickness_min = 100.f;
	out_iridescence->iridescence_thickness_max = 400.f;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "iridescenceFactor") == 0)
		{
			++i;
			out_iridescence->iridescence_factor = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "iridescenceTexture") == 0)
		{
			i = PX_Gltf_ParseJsonTextureView(options, tokens, i + 1, json_chunk, &out_iridescence->iridescence_texture);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "iridescenceIor") == 0)
		{
			++i;
			out_iridescence->iridescence_ior = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "iridescenceThicknessMinimum") == 0)
		{
			++i;
			out_iridescence->iridescence_thickness_min = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "iridescenceThicknessMaximum") == 0)
		{
			++i;
			out_iridescence->iridescence_thickness_max = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "iridescenceThicknessTexture") == 0)
		{
			i = PX_Gltf_ParseJsonTextureView(options, tokens, i + 1, json_chunk, &out_iridescence->iridescence_thickness_texture);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i + 1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonDiffuseTransmission(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_DiffuseTransmission* out_diff_transmission)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
	px_int size = tokens[i].size;
	++i;

	// Defaults
	PX_Gltf_FillFloatArray(out_diff_transmission->diffuse_transmission_color_factor, 3, 1.0f);
	out_diff_transmission->diffuse_transmission_factor = 0.f;
	
	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "diffuseTransmissionFactor") == 0)
		{
			++i;
			out_diff_transmission->diffuse_transmission_factor = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "diffuseTransmissionTexture") == 0)
		{
			i = PX_Gltf_ParseJsonTextureView(options, tokens, i + 1, json_chunk, &out_diff_transmission->diffuse_transmission_texture);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "diffuseTransmissionColorFactor") == 0)
		{
			i = PX_Gltf_ParseJsonFloatArray(tokens, i + 1, json_chunk, out_diff_transmission->diffuse_transmission_color_factor, 3);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "diffuseTransmissionColorTexture") == 0)
		{
			i = PX_Gltf_ParseJsonTextureView(options, tokens, i + 1, json_chunk, &out_diff_transmission->diffuse_transmission_color_texture);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i + 1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonAnisotropy(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Anisotropy* out_anisotropy)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "anisotropyStrength") == 0)
		{
			++i;
			out_anisotropy->anisotropy_strength = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "anisotropyRotation") == 0)
		{
			++i;
			out_anisotropy->anisotropy_rotation = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "anisotropyTexture") == 0)
		{
			i = PX_Gltf_ParseJsonTextureView(options, tokens, i + 1, json_chunk, &out_anisotropy->anisotropy_texture);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i + 1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonDispersion(PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Dispersion* out_dispersion)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "dispersion") == 0)
		{
			++i;
			out_dispersion->dispersion = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i + 1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonImage(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Image* out_image)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "uri") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_image->uri);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "bufferView") == 0)
		{
			++i;
			out_image->buffer_view = PX_GLTF_PTRINDEX(PX_Gltf_BufferView, PX_Gltf_JsonToInt(tokens + i, json_chunk));
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "mimeType") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_image->mime_type);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "name") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_image->name);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
		{
			i = PX_Gltf_ParseJsonExtras(options, tokens, i + 1, json_chunk, &out_image->extras);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extensions") == 0)
		{
			i = PX_Gltf_ParseJsonUnprocessedExtensions(options, tokens, i, json_chunk, &out_image->extensions_count, &out_image->extensions);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i + 1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonSampler(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Sampler* out_sampler)
{
	(px_void)options;
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	out_sampler->wrap_s = PX_GLTF_WRAP_MODE_REPEAT;
	out_sampler->wrap_t = PX_GLTF_WRAP_MODE_REPEAT;

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "name") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_sampler->name);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "magFilter") == 0)
		{
			++i;
			out_sampler->mag_filter
				= (PX_GLTF_FILTER_TYPE)PX_Gltf_JsonToInt(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "minFilter") == 0)
		{
			++i;
			out_sampler->min_filter
				= (PX_GLTF_FILTER_TYPE)PX_Gltf_JsonToInt(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "wrapS") == 0)
		{
			++i;
			out_sampler->wrap_s
				= (PX_GLTF_WRAP_MODE)PX_Gltf_JsonToInt(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "wrapT") == 0)
		{
			++i;
			out_sampler->wrap_t
				= (PX_GLTF_WRAP_MODE)PX_Gltf_JsonToInt(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
		{
			i = PX_Gltf_ParseJsonExtras(options, tokens, i + 1, json_chunk, &out_sampler->extras);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extensions") == 0)
		{
			i = PX_Gltf_ParseJsonUnprocessedExtensions(options, tokens, i, json_chunk, &out_sampler->extensions_count, &out_sampler->extensions);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i + 1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonTexture(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Texture* out_texture)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "name") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_texture->name);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "sampler") == 0)
		{
			++i;
			out_texture->sampler = PX_GLTF_PTRINDEX(PX_Gltf_Sampler, PX_Gltf_JsonToInt(tokens + i, json_chunk));
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "source") == 0)
		{
			++i;
			out_texture->image = PX_GLTF_PTRINDEX(PX_Gltf_Image, PX_Gltf_JsonToInt(tokens + i, json_chunk));
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
		{
			i = PX_Gltf_ParseJsonExtras(options, tokens, i + 1, json_chunk, &out_texture->extras);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extensions") == 0)
		{
			++i;

			PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
			if (out_texture->extensions)
			{
				return PX_GLTF_ERROR_JSON;
			}

			px_int extensions_size = tokens[i].size;
			++i;
			out_texture->extensions = (PX_Gltf_Extension*)PX_Gltf_Calloc(options, sizeof(PX_Gltf_Extension), extensions_size);
			out_texture->extensions_count = 0;

			if (!out_texture->extensions)
			{
				return PX_GLTF_ERROR_NOMEM;
			}

			for (px_int k = 0; k < extensions_size; ++k)
			{
				PX_GLTF_CHECK_KEY(tokens[i]);

				if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "KHR_texture_basisu") == 0)
				{
					out_texture->has_basisu = 1;
					++i;
					PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
					px_int num_properties = tokens[i].size;
					++i;

					for (px_int t = 0; t < num_properties; ++t)
					{
						PX_GLTF_CHECK_KEY(tokens[i]);

						if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "source") == 0)
						{
							++i;
							out_texture->basisu_image = PX_GLTF_PTRINDEX(PX_Gltf_Image, PX_Gltf_JsonToInt(tokens + i, json_chunk));
							++i;
						}
						else
						{
							i = PX_Gltf_SkipJson(tokens, i + 1);
						}
						if (i < 0)
						{
							return i;
						}
					}
				}
				else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "EXT_texture_webp") == 0)
				{
					out_texture->has_webp = 1;
					++i;
					PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
					px_int num_properties = tokens[i].size;
					++i;

					for (px_int t = 0; t < num_properties; ++t)
					{
						PX_GLTF_CHECK_KEY(tokens[i]);

						if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "source") == 0)
						{
							++i;
							out_texture->webp_image = PX_GLTF_PTRINDEX(PX_Gltf_Image, PX_Gltf_JsonToInt(tokens + i, json_chunk));
							++i;
						}
						else
						{
							i = PX_Gltf_SkipJson(tokens, i + 1);
						}
						if (i < 0)
						{
							return i;
						}
					}
				}
				else
				{
					i = PX_Gltf_ParseJsonUnprocessedExtension(options, tokens, i, json_chunk, &(out_texture->extensions[out_texture->extensions_count++]));
				}

				if (i < 0)
				{
					return i;
				}
			}
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i + 1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonMaterial(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Material* out_material)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	PX_Gltf_FillFloatArray(out_material->pbr_metallic_roughness.base_color_factor, 4, 1.0f);
	out_material->pbr_metallic_roughness.metallic_factor = 1.0f;
	out_material->pbr_metallic_roughness.roughness_factor = 1.0f;

	PX_Gltf_FillFloatArray(out_material->pbr_specular_glossiness.diffuse_factor, 4, 1.0f);
	PX_Gltf_FillFloatArray(out_material->pbr_specular_glossiness.specular_factor, 3, 1.0f);
	out_material->pbr_specular_glossiness.glossiness_factor = 1.0f;

	PX_Gltf_FillFloatArray(out_material->volume.attenuation_color, 3, 1.0f);
	out_material->volume.attenuation_distance = PX_GLTF_FLT_MAX;

	out_material->alpha_cutoff = 0.5f;

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "name") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_material->name);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "pbrMetallicRoughness") == 0)
		{
			out_material->has_pbr_metallic_roughness = 1;
			i = PX_Gltf_ParseJsonPbrMetallicRoughness(options, tokens, i + 1, json_chunk, &out_material->pbr_metallic_roughness);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "emissiveFactor") == 0)
		{
			i = PX_Gltf_ParseJsonFloatArray(tokens, i + 1, json_chunk, out_material->emissive_factor, 3);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "normalTexture") == 0)
		{
			i = PX_Gltf_ParseJsonTextureView(options, tokens, i + 1, json_chunk,
				&out_material->normal_texture);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "occlusionTexture") == 0)
		{
			i = PX_Gltf_ParseJsonTextureView(options, tokens, i + 1, json_chunk,
				&out_material->occlusion_texture);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "emissiveTexture") == 0)
		{
			i = PX_Gltf_ParseJsonTextureView(options, tokens, i + 1, json_chunk,
				&out_material->emissive_texture);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "alphaMode") == 0)
		{
			++i;
			if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "OPAQUE") == 0)
			{
				out_material->alpha_mode = PX_GLTF_ALPHA_MODE_OPAQUE;
			}
			else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "MASK") == 0)
			{
				out_material->alpha_mode = PX_GLTF_ALPHA_MODE_MASK;
			}
			else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "BLEND") == 0)
			{
				out_material->alpha_mode = PX_GLTF_ALPHA_MODE_BLEND;
			}
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "alphaCutoff") == 0)
		{
			++i;
			out_material->alpha_cutoff = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "doubleSided") == 0)
		{
			++i;
			out_material->double_sided =
				PX_Gltf_JsonToBool(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
		{
			i = PX_Gltf_ParseJsonExtras(options, tokens, i + 1, json_chunk, &out_material->extras);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extensions") == 0)
		{
			++i;

			PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
			if(out_material->extensions)
			{
				return PX_GLTF_ERROR_JSON;
			}

			px_int extensions_size = tokens[i].size;
			++i;
			out_material->extensions = (PX_Gltf_Extension*)PX_Gltf_Calloc(options, sizeof(PX_Gltf_Extension), extensions_size);
			out_material->extensions_count= 0;

			if (!out_material->extensions)
			{
				return PX_GLTF_ERROR_NOMEM;
			}

			for (px_int k = 0; k < extensions_size; ++k)
			{
				PX_GLTF_CHECK_KEY(tokens[i]);

				if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "KHR_materials_pbrSpecularGlossiness") == 0)
				{
					out_material->has_pbr_specular_glossiness = 1;
					i = PX_Gltf_ParseJsonPbrSpecularGlossiness(options, tokens, i + 1, json_chunk, &out_material->pbr_specular_glossiness);
				}
				else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "KHR_materials_unlit") == 0)
				{
					out_material->unlit = 1;
					i = PX_Gltf_SkipJson(tokens, i+1);
				}
				else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "KHR_materials_clearcoat") == 0)
				{
					out_material->has_clearcoat = 1;
					i = PX_Gltf_ParseJsonClearcoat(options, tokens, i + 1, json_chunk, &out_material->clearcoat);
				}
				else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "KHR_materials_ior") == 0)
				{
					out_material->has_ior = 1;
					i = PX_Gltf_ParseJsonIor(tokens, i + 1, json_chunk, &out_material->ior);
				}
				else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "KHR_materials_specular") == 0)
				{
					out_material->has_specular = 1;
					i = PX_Gltf_ParseJsonSpecular(options, tokens, i + 1, json_chunk, &out_material->specular);
				}
				else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "KHR_materials_transmission") == 0)
				{
					out_material->has_transmission = 1;
					i = PX_Gltf_ParseJsonTransmission(options, tokens, i + 1, json_chunk, &out_material->transmission);
				}
				else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "KHR_materials_volume") == 0)
				{
					out_material->has_volume = 1;
					i = PX_Gltf_ParseJsonVolume(options, tokens, i + 1, json_chunk, &out_material->volume);
				}
				else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "KHR_materials_sheen") == 0)
				{
					out_material->has_sheen = 1;
					i = PX_Gltf_ParseJsonSheen(options, tokens, i + 1, json_chunk, &out_material->sheen);
				}
				else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "KHR_materials_emissive_strength") == 0)
				{
					out_material->has_emissive_strength = 1;
					i = PX_Gltf_ParseJsonEmissiveStrength(tokens, i + 1, json_chunk, &out_material->emissive_strength);
				}
				else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "KHR_materials_iridescence") == 0)
				{
					out_material->has_iridescence = 1;
					i = PX_Gltf_ParseJsonIridescence(options, tokens, i + 1, json_chunk, &out_material->iridescence);
				}
				else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "KHR_materials_diffuse_transmission") == 0)
				{
					out_material->has_diffuse_transmission = 1;
					i = PX_Gltf_ParseJsonDiffuseTransmission(options, tokens, i + 1, json_chunk, &out_material->diffuse_transmission);
				}
				else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "KHR_materials_anisotropy") == 0)
				{
					out_material->has_anisotropy = 1;
					i = PX_Gltf_ParseJsonAnisotropy(options, tokens, i + 1, json_chunk, &out_material->anisotropy);
				}
				else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "KHR_materials_dispersion") == 0)
				{
					out_material->has_dispersion = 1;
					i = PX_Gltf_ParseJsonDispersion(tokens, i + 1, json_chunk, &out_material->dispersion);
				}
				else
				{
					i = PX_Gltf_ParseJsonUnprocessedExtension(options, tokens, i, json_chunk, &(out_material->extensions[out_material->extensions_count++]));
				}

				if (i < 0)
				{
					return i;
				}
			}
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonAccessors(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Data* out_data)
{
	i = PX_Gltf_ParseJsonArray(options, tokens, i, json_chunk, sizeof(PX_Gltf_Accessor), (px_void**)&out_data->accessors, &out_data->accessors_count);
	if (i < 0)
	{
		return i;
	}

	for (px_gltf_size j = 0; j < out_data->accessors_count; ++j)
	{
		i = PX_Gltf_ParseJsonAccessor(options, tokens, i, json_chunk, &out_data->accessors[j]);
		if (i < 0)
		{
			return i;
		}
	}
	return i;
}

static px_int PX_Gltf_ParseJsonMaterials(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Data* out_data)
{
	i = PX_Gltf_ParseJsonArray(options, tokens, i, json_chunk, sizeof(PX_Gltf_Material), (px_void**)&out_data->materials, &out_data->materials_count);
	if (i < 0)
	{
		return i;
	}

	for (px_gltf_size j = 0; j < out_data->materials_count; ++j)
	{
		i = PX_Gltf_ParseJsonMaterial(options, tokens, i, json_chunk, &out_data->materials[j]);
		if (i < 0)
		{
			return i;
		}
	}
	return i;
}

static px_int PX_Gltf_ParseJsonImages(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Data* out_data)
{
	i = PX_Gltf_ParseJsonArray(options, tokens, i, json_chunk, sizeof(PX_Gltf_Image), (px_void**)&out_data->images, &out_data->images_count);
	if (i < 0)
	{
		return i;
	}

	for (px_gltf_size j = 0; j < out_data->images_count; ++j)
	{
		i = PX_Gltf_ParseJsonImage(options, tokens, i, json_chunk, &out_data->images[j]);
		if (i < 0)
		{
			return i;
		}
	}
	return i;
}

static px_int PX_Gltf_ParseJsonTextures(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Data* out_data)
{
	i = PX_Gltf_ParseJsonArray(options, tokens, i, json_chunk, sizeof(PX_Gltf_Texture), (px_void**)&out_data->textures, &out_data->textures_count);
	if (i < 0)
	{
		return i;
	}

	for (px_gltf_size j = 0; j < out_data->textures_count; ++j)
	{
		i = PX_Gltf_ParseJsonTexture(options, tokens, i, json_chunk, &out_data->textures[j]);
		if (i < 0)
		{
			return i;
		}
	}
	return i;
}

static px_int PX_Gltf_ParseJsonSamplers(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Data* out_data)
{
	i = PX_Gltf_ParseJsonArray(options, tokens, i, json_chunk, sizeof(PX_Gltf_Sampler), (px_void**)&out_data->samplers, &out_data->samplers_count);
	if (i < 0)
	{
		return i;
	}

	for (px_gltf_size j = 0; j < out_data->samplers_count; ++j)
	{
		i = PX_Gltf_ParseJsonSampler(options, tokens, i, json_chunk, &out_data->samplers[j]);
		if (i < 0)
		{
			return i;
		}
	}
	return i;
}

static px_int PX_Gltf_ParseJsonMeshoptCompression(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_MeshoptCompression* out_meshopt_compression)
{
	(px_void)options;
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "buffer") == 0)
		{
			++i;
			out_meshopt_compression->buffer = PX_GLTF_PTRINDEX(PX_Gltf_Buffer, PX_Gltf_JsonToInt(tokens + i, json_chunk));
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "byteOffset") == 0)
		{
			++i;
			out_meshopt_compression->offset = PX_Gltf_JsonToSize(tokens+i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "byteLength") == 0)
		{
			++i;
			out_meshopt_compression->size = PX_Gltf_JsonToSize(tokens+i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "byteStride") == 0)
		{
			++i;
			out_meshopt_compression->stride = PX_Gltf_JsonToSize(tokens+i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "count") == 0)
		{
			++i;
			out_meshopt_compression->count = PX_Gltf_JsonToSize(tokens+i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "mode") == 0)
		{
			++i;
			if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "ATTRIBUTES") == 0)
			{
				out_meshopt_compression->mode = PX_GLTF_MESHOPT_COMPRESSION_MODE_ATTRIBUTES;
			}
			else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "TRIANGLES") == 0)
			{
				out_meshopt_compression->mode = PX_GLTF_MESHOPT_COMPRESSION_MODE_TRIANGLES;
			}
			else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "INDICES") == 0)
			{
				out_meshopt_compression->mode = PX_GLTF_MESHOPT_COMPRESSION_MODE_INDICES;
			}
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "filter") == 0)
		{
			++i;
			if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "NONE") == 0)
			{
				out_meshopt_compression->filter = PX_GLTF_MESHOPT_COMPRESSION_FILTER_NONE;
			}
			else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "OCTAHEDRAL") == 0)
			{
				out_meshopt_compression->filter = PX_GLTF_MESHOPT_COMPRESSION_FILTER_OCTAHEDRAL;
			}
			else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "QUATERNION") == 0)
			{
				out_meshopt_compression->filter = PX_GLTF_MESHOPT_COMPRESSION_FILTER_QUATERNION;
			}
			else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "EXPONENTIAL") == 0)
			{
				out_meshopt_compression->filter = PX_GLTF_MESHOPT_COMPRESSION_FILTER_EXPONENTIAL;
			}
			else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "COLOR") == 0)
			{
				out_meshopt_compression->filter = PX_GLTF_MESHOPT_COMPRESSION_FILTER_COLOR;
			}
			++i;
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonBufferView(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_BufferView* out_buffer_view)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "name") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_buffer_view->name);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "buffer") == 0)
		{
			++i;
			out_buffer_view->buffer = PX_GLTF_PTRINDEX(PX_Gltf_Buffer, PX_Gltf_JsonToInt(tokens + i, json_chunk));
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "byteOffset") == 0)
		{
			++i;
			out_buffer_view->offset =
					PX_Gltf_JsonToSize(tokens+i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "byteLength") == 0)
		{
			++i;
			out_buffer_view->size =
					PX_Gltf_JsonToSize(tokens+i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "byteStride") == 0)
		{
			++i;
			out_buffer_view->stride =
					PX_Gltf_JsonToSize(tokens+i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "target") == 0)
		{
			++i;
			px_int type = PX_Gltf_JsonToInt(tokens+i, json_chunk);
			switch (type)
			{
			case 34962:
				type = PX_GLTF_BUFFER_VIEW_TYPE_VERTICES;
				break;
			case 34963:
				type = PX_GLTF_BUFFER_VIEW_TYPE_INDICES;
				break;
			default:
				type = PX_GLTF_BUFFER_VIEW_TYPE_INVALID;
				break;
			}
			out_buffer_view->type = (PX_GLTF_BUFFER_VIEW_TYPE)type;
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
		{
			i = PX_Gltf_ParseJsonExtras(options, tokens, i + 1, json_chunk, &out_buffer_view->extras);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extensions") == 0)
		{
			++i;

			PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
			if(out_buffer_view->extensions)
			{
				return PX_GLTF_ERROR_JSON;
			}

			px_int extensions_size = tokens[i].size;
			out_buffer_view->extensions_count = 0;
			out_buffer_view->extensions = (PX_Gltf_Extension*)PX_Gltf_Calloc(options, sizeof(PX_Gltf_Extension), extensions_size);

			if (!out_buffer_view->extensions)
			{
				return PX_GLTF_ERROR_NOMEM;
			}

			++i;
			for (px_int k = 0; k < extensions_size; ++k)
			{
				PX_GLTF_CHECK_KEY(tokens[i]);

				if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "EXT_meshopt_compression") == 0)
				{
					out_buffer_view->has_meshopt_compression = 1;
					i = PX_Gltf_ParseJsonMeshoptCompression(options, tokens, i + 1, json_chunk, &out_buffer_view->meshopt_compression);
				}
				else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "KHR_meshopt_compression") == 0)
				{
					out_buffer_view->has_meshopt_compression = 1;
					out_buffer_view->meshopt_compression.is_khr = 1;
					i = PX_Gltf_ParseJsonMeshoptCompression(options, tokens, i + 1, json_chunk, &out_buffer_view->meshopt_compression);
				}
				else
				{
					i = PX_Gltf_ParseJsonUnprocessedExtension(options, tokens, i, json_chunk, &(out_buffer_view->extensions[out_buffer_view->extensions_count++]));
				}

				if (i < 0)
				{
					return i;
				}
			}
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonBufferViews(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Data* out_data)
{
	i = PX_Gltf_ParseJsonArray(options, tokens, i, json_chunk, sizeof(PX_Gltf_BufferView), (px_void**)&out_data->buffer_views, &out_data->buffer_views_count);
	if (i < 0)
	{
		return i;
	}

	for (px_gltf_size j = 0; j < out_data->buffer_views_count; ++j)
	{
		i = PX_Gltf_ParseJsonBufferView(options, tokens, i, json_chunk, &out_data->buffer_views[j]);
		if (i < 0)
		{
			return i;
		}
	}
	return i;
}

static px_int PX_Gltf_ParseJsonBuffer(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Buffer* out_buffer)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "name") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_buffer->name);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "byteLength") == 0)
		{
			++i;
			out_buffer->size =
					PX_Gltf_JsonToSize(tokens+i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "uri") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_buffer->uri);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
		{
			i = PX_Gltf_ParseJsonExtras(options, tokens, i + 1, json_chunk, &out_buffer->extras);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extensions") == 0)
		{
			i = PX_Gltf_ParseJsonUnprocessedExtensions(options, tokens, i, json_chunk, &out_buffer->extensions_count, &out_buffer->extensions);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonBuffers(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Data* out_data)
{
	i = PX_Gltf_ParseJsonArray(options, tokens, i, json_chunk, sizeof(PX_Gltf_Buffer), (px_void**)&out_data->buffers, &out_data->buffers_count);
	if (i < 0)
	{
		return i;
	}

	for (px_gltf_size j = 0; j < out_data->buffers_count; ++j)
	{
		i = PX_Gltf_ParseJsonBuffer(options, tokens, i, json_chunk, &out_data->buffers[j]);
		if (i < 0)
		{
			return i;
		}
	}
	return i;
}

static px_int PX_Gltf_ParseJsonSkin(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Skin* out_skin)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "name") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_skin->name);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "joints") == 0)
		{
			i = PX_Gltf_ParseJsonArray(options, tokens, i + 1, json_chunk, sizeof(PX_Gltf_Node*), (px_void**)&out_skin->joints, &out_skin->joints_count);
			if (i < 0)
			{
				return i;
			}

			for (px_gltf_size k = 0; k < out_skin->joints_count; ++k)
			{
				out_skin->joints[k] = PX_GLTF_PTRINDEX(PX_Gltf_Node, PX_Gltf_JsonToInt(tokens + i, json_chunk));
				++i;
			}
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "skeleton") == 0)
		{
			++i;
			PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_PRIMITIVE);
			out_skin->skeleton = PX_GLTF_PTRINDEX(PX_Gltf_Node, PX_Gltf_JsonToInt(tokens + i, json_chunk));
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "inverseBindMatrices") == 0)
		{
			++i;
			PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_PRIMITIVE);
			out_skin->inverse_bind_matrices = PX_GLTF_PTRINDEX(PX_Gltf_Accessor, PX_Gltf_JsonToInt(tokens + i, json_chunk));
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
		{
			i = PX_Gltf_ParseJsonExtras(options, tokens, i + 1, json_chunk, &out_skin->extras);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extensions") == 0)
		{
			i = PX_Gltf_ParseJsonUnprocessedExtensions(options, tokens, i, json_chunk, &out_skin->extensions_count, &out_skin->extensions);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonSkins(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Data* out_data)
{
	i = PX_Gltf_ParseJsonArray(options, tokens, i, json_chunk, sizeof(PX_Gltf_Skin), (px_void**)&out_data->skins, &out_data->skins_count);
	if (i < 0)
	{
		return i;
	}

	for (px_gltf_size j = 0; j < out_data->skins_count; ++j)
	{
		i = PX_Gltf_ParseJsonSkin(options, tokens, i, json_chunk, &out_data->skins[j]);
		if (i < 0)
		{
			return i;
		}
	}
	return i;
}

static px_int PX_Gltf_ParseJsonCamera(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Camera* out_camera)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "name") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_camera->name);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "perspective") == 0)
		{
			++i;

			PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

			px_int data_size = tokens[i].size;
			++i;

			if (out_camera->type != PX_GLTF_CAMERA_TYPE_INVALID)
			{
				return PX_GLTF_ERROR_JSON;
			}

			out_camera->type = PX_GLTF_CAMERA_TYPE_PERSPECTIVE;

			for (px_int k = 0; k < data_size; ++k)
			{
				PX_GLTF_CHECK_KEY(tokens[i]);

				if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "aspectRatio") == 0)
				{
					++i;
					out_camera->data.perspective.has_aspect_ratio = 1;
					out_camera->data.perspective.aspect_ratio = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
					++i;
				}
				else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "yfov") == 0)
				{
					++i;
					out_camera->data.perspective.yfov = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
					++i;
				}
				else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "zfar") == 0)
				{
					++i;
					out_camera->data.perspective.has_zfar = 1;
					out_camera->data.perspective.zfar = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
					++i;
				}
				else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "znear") == 0)
				{
					++i;
					out_camera->data.perspective.znear = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
					++i;
				}
				else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
				{
					i = PX_Gltf_ParseJsonExtras(options, tokens, i + 1, json_chunk, &out_camera->data.perspective.extras);
				}
				else
				{
					i = PX_Gltf_SkipJson(tokens, i+1);
				}

				if (i < 0)
				{
					return i;
				}
			}
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "orthographic") == 0)
		{
			++i;

			PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

			px_int data_size = tokens[i].size;
			++i;

			if (out_camera->type != PX_GLTF_CAMERA_TYPE_INVALID)
			{
				return PX_GLTF_ERROR_JSON;
			}

			out_camera->type = PX_GLTF_CAMERA_TYPE_ORTHOGRAPHIC;

			for (px_int k = 0; k < data_size; ++k)
			{
				PX_GLTF_CHECK_KEY(tokens[i]);

				if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "xmag") == 0)
				{
					++i;
					out_camera->data.orthographic.xmag = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
					++i;
				}
				else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "ymag") == 0)
				{
					++i;
					out_camera->data.orthographic.ymag = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
					++i;
				}
				else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "zfar") == 0)
				{
					++i;
					out_camera->data.orthographic.zfar = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
					++i;
				}
				else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "znear") == 0)
				{
					++i;
					out_camera->data.orthographic.znear = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
					++i;
				}
				else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
				{
					i = PX_Gltf_ParseJsonExtras(options, tokens, i + 1, json_chunk, &out_camera->data.orthographic.extras);
				}
				else
				{
					i = PX_Gltf_SkipJson(tokens, i+1);
				}

				if (i < 0)
				{
					return i;
				}
			}
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
		{
			i = PX_Gltf_ParseJsonExtras(options, tokens, i + 1, json_chunk, &out_camera->extras);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extensions") == 0)
		{
			i = PX_Gltf_ParseJsonUnprocessedExtensions(options, tokens, i, json_chunk, &out_camera->extensions_count, &out_camera->extensions);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonCameras(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Data* out_data)
{
	i = PX_Gltf_ParseJsonArray(options, tokens, i, json_chunk, sizeof(PX_Gltf_Camera), (px_void**)&out_data->cameras, &out_data->cameras_count);
	if (i < 0)
	{
		return i;
	}

	for (px_gltf_size j = 0; j < out_data->cameras_count; ++j)
	{
		i = PX_Gltf_ParseJsonCamera(options, tokens, i, json_chunk, &out_data->cameras[j]);
		if (i < 0)
		{
			return i;
		}
	}
	return i;
}

static px_int PX_Gltf_ParseJsonLight(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Light* out_light)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	out_light->color[0] = 1.f;
	out_light->color[1] = 1.f;
	out_light->color[2] = 1.f;
	out_light->intensity = 1.f;

	out_light->spot_inner_cone_angle = 0.f;
	out_light->spot_outer_cone_angle = 3.1415926535f / 4.0f;

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "name") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_light->name);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "color") == 0)
		{
			i = PX_Gltf_ParseJsonFloatArray(tokens, i + 1, json_chunk, out_light->color, 3);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "intensity") == 0)
		{
			++i;
			out_light->intensity = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "type") == 0)
		{
			++i;
			if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "directional") == 0)
			{
				out_light->type = PX_GLTF_LIGHT_TYPE_DIRECTIONAL;
			}
			else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "point") == 0)
			{
				out_light->type = PX_GLTF_LIGHT_TYPE_POINT;
			}
			else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "spot") == 0)
			{
				out_light->type = PX_GLTF_LIGHT_TYPE_SPOT;
			}
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "range") == 0)
		{
			++i;
			out_light->range = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "spot") == 0)
		{
			++i;

			PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

			px_int data_size = tokens[i].size;
			++i;

			for (px_int k = 0; k < data_size; ++k)
			{
				PX_GLTF_CHECK_KEY(tokens[i]);

				if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "innerConeAngle") == 0)
				{
					++i;
					out_light->spot_inner_cone_angle = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
					++i;
				}
				else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "outerConeAngle") == 0)
				{
					++i;
					out_light->spot_outer_cone_angle = PX_Gltf_JsonToFloat(tokens + i, json_chunk);
					++i;
				}
				else
				{
					i = PX_Gltf_SkipJson(tokens, i+1);
				}

				if (i < 0)
				{
					return i;
				}
			}
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
		{
			i = PX_Gltf_ParseJsonExtras(options, tokens, i + 1, json_chunk, &out_light->extras);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonLights(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Data* out_data)
{
	i = PX_Gltf_ParseJsonArray(options, tokens, i, json_chunk, sizeof(PX_Gltf_Light), (px_void**)&out_data->lights, &out_data->lights_count);
	if (i < 0)
	{
		return i;
	}

	for (px_gltf_size j = 0; j < out_data->lights_count; ++j)
	{
		i = PX_Gltf_ParseJsonLight(options, tokens, i, json_chunk, &out_data->lights[j]);
		if (i < 0)
		{
			return i;
		}
	}
	return i;
}

static px_int PX_Gltf_ParseJsonNode(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Node* out_node)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	out_node->rotation[3] = 1.0f;
	out_node->scale[0] = 1.0f;
	out_node->scale[1] = 1.0f;
	out_node->scale[2] = 1.0f;
	out_node->matrix[0] = 1.0f;
	out_node->matrix[5] = 1.0f;
	out_node->matrix[10] = 1.0f;
	out_node->matrix[15] = 1.0f;

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "name") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_node->name);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "children") == 0)
		{
			i = PX_Gltf_ParseJsonArray(options, tokens, i + 1, json_chunk, sizeof(PX_Gltf_Node*), (px_void**)&out_node->children, &out_node->children_count);
			if (i < 0)
			{
				return i;
			}

			for (px_gltf_size k = 0; k < out_node->children_count; ++k)
			{
				out_node->children[k] = PX_GLTF_PTRINDEX(PX_Gltf_Node, PX_Gltf_JsonToInt(tokens + i, json_chunk));
				++i;
			}
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "mesh") == 0)
		{
			++i;
			PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_PRIMITIVE);
			out_node->mesh = PX_GLTF_PTRINDEX(PX_Gltf_Mesh, PX_Gltf_JsonToInt(tokens + i, json_chunk));
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "skin") == 0)
		{
			++i;
			PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_PRIMITIVE);
			out_node->skin = PX_GLTF_PTRINDEX(PX_Gltf_Skin, PX_Gltf_JsonToInt(tokens + i, json_chunk));
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "camera") == 0)
		{
			++i;
			PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_PRIMITIVE);
			out_node->camera = PX_GLTF_PTRINDEX(PX_Gltf_Camera, PX_Gltf_JsonToInt(tokens + i, json_chunk));
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "translation") == 0)
		{
			out_node->has_translation = 1;
			i = PX_Gltf_ParseJsonFloatArray(tokens, i + 1, json_chunk, out_node->translation, 3);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "rotation") == 0)
		{
			out_node->has_rotation = 1;
			i = PX_Gltf_ParseJsonFloatArray(tokens, i + 1, json_chunk, out_node->rotation, 4);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "scale") == 0)
		{
			out_node->has_scale = 1;
			i = PX_Gltf_ParseJsonFloatArray(tokens, i + 1, json_chunk, out_node->scale, 3);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "matrix") == 0)
		{
			out_node->has_matrix = 1;
			i = PX_Gltf_ParseJsonFloatArray(tokens, i + 1, json_chunk, out_node->matrix, 16);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "weights") == 0)
		{
			i = PX_Gltf_ParseJsonArray(options, tokens, i + 1, json_chunk, sizeof(px_float), (px_void**)&out_node->weights, &out_node->weights_count);
			if (i < 0)
			{
				return i;
			}

			i = PX_Gltf_ParseJsonFloatArray(tokens, i - 1, json_chunk, out_node->weights, (px_int)out_node->weights_count);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
		{
			i = PX_Gltf_ParseJsonExtras(options, tokens, i + 1, json_chunk, &out_node->extras);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extensions") == 0)
		{
			++i;

			PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
			if(out_node->extensions)
			{
				return PX_GLTF_ERROR_JSON;
			}

			px_int extensions_size = tokens[i].size;
			out_node->extensions_count= 0;
			out_node->extensions = (PX_Gltf_Extension*)PX_Gltf_Calloc(options, sizeof(PX_Gltf_Extension), extensions_size);

			if (!out_node->extensions)
			{
				return PX_GLTF_ERROR_NOMEM;
			}

			++i;

			for (px_int k = 0; k < extensions_size; ++k)
			{
				PX_GLTF_CHECK_KEY(tokens[i]);

				if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "KHR_lights_punctual") == 0)
				{
					++i;

					PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

					px_int data_size = tokens[i].size;
					++i;

					for (px_int m = 0; m < data_size; ++m)
					{
						PX_GLTF_CHECK_KEY(tokens[i]);

						if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "light") == 0)
						{
							++i;
							PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_PRIMITIVE);
							out_node->light = PX_GLTF_PTRINDEX(PX_Gltf_Light, PX_Gltf_JsonToInt(tokens + i, json_chunk));
							++i;
						}
						else
						{
							i = PX_Gltf_SkipJson(tokens, i + 1);
						}

						if (i < 0)
						{
							return i;
						}
					}
				}
				else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "EXT_mesh_gpu_instancing") == 0)
				{
					out_node->has_mesh_gpu_instancing = 1;
					i = PX_Gltf_ParseJsonMeshGpuInstancing(options, tokens, i + 1, json_chunk, &out_node->mesh_gpu_instancing);
				}
				else
				{
					i = PX_Gltf_ParseJsonUnprocessedExtension(options, tokens, i, json_chunk, &(out_node->extensions[out_node->extensions_count++]));
				}

				if (i < 0)
				{
					return i;
				}
			}
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonNodes(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Data* out_data)
{
	i = PX_Gltf_ParseJsonArray(options, tokens, i, json_chunk, sizeof(PX_Gltf_Node), (px_void**)&out_data->nodes, &out_data->nodes_count);
	if (i < 0)
	{
		return i;
	}

	for (px_gltf_size j = 0; j < out_data->nodes_count; ++j)
	{
		i = PX_Gltf_ParseJsonNode(options, tokens, i, json_chunk, &out_data->nodes[j]);
		if (i < 0)
		{
			return i;
		}
	}
	return i;
}

static px_int PX_Gltf_ParseJsonScene(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Scene* out_scene)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "name") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_scene->name);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "nodes") == 0)
		{
			i = PX_Gltf_ParseJsonArray(options, tokens, i + 1, json_chunk, sizeof(PX_Gltf_Node*), (px_void**)&out_scene->nodes, &out_scene->nodes_count);
			if (i < 0)
			{
				return i;
			}

			for (px_gltf_size k = 0; k < out_scene->nodes_count; ++k)
			{
				out_scene->nodes[k] = PX_GLTF_PTRINDEX(PX_Gltf_Node, PX_Gltf_JsonToInt(tokens + i, json_chunk));
				++i;
			}
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
		{
			i = PX_Gltf_ParseJsonExtras(options, tokens, i + 1, json_chunk, &out_scene->extras);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extensions") == 0)
		{
			i = PX_Gltf_ParseJsonUnprocessedExtensions(options, tokens, i, json_chunk, &out_scene->extensions_count, &out_scene->extensions);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonScenes(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Data* out_data)
{
	i = PX_Gltf_ParseJsonArray(options, tokens, i, json_chunk, sizeof(PX_Gltf_Scene), (px_void**)&out_data->scenes, &out_data->scenes_count);
	if (i < 0)
	{
		return i;
	}

	for (px_gltf_size j = 0; j < out_data->scenes_count; ++j)
	{
		i = PX_Gltf_ParseJsonScene(options, tokens, i, json_chunk, &out_data->scenes[j]);
		if (i < 0)
		{
			return i;
		}
	}
	return i;
}

static px_int PX_Gltf_ParseJsonAnimationSampler(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_AnimationSampler* out_sampler)
{
	(px_void)options;
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "input") == 0)
		{
			++i;
			out_sampler->input = PX_GLTF_PTRINDEX(PX_Gltf_Accessor, PX_Gltf_JsonToInt(tokens + i, json_chunk));
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "output") == 0)
		{
			++i;
			out_sampler->output = PX_GLTF_PTRINDEX(PX_Gltf_Accessor, PX_Gltf_JsonToInt(tokens + i, json_chunk));
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "interpolation") == 0)
		{
			++i;
			if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "LINEAR") == 0)
			{
				out_sampler->interpolation = PX_GLTF_INTERPOLATION_TYPE_LINEAR;
			}
			else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "STEP") == 0)
			{
				out_sampler->interpolation = PX_GLTF_INTERPOLATION_TYPE_STEP;
			}
			else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "CUBICSPLINE") == 0)
			{
				out_sampler->interpolation = PX_GLTF_INTERPOLATION_TYPE_CUBIC_SPLINE;
			}
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
		{
			i = PX_Gltf_ParseJsonExtras(options, tokens, i + 1, json_chunk, &out_sampler->extras);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extensions") == 0)
		{
			i = PX_Gltf_ParseJsonUnprocessedExtensions(options, tokens, i, json_chunk, &out_sampler->extensions_count, &out_sampler->extensions);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonAnimationChannel(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_AnimationChannel* out_channel)
{
	(px_void)options;
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "sampler") == 0)
		{
			++i;
			out_channel->sampler = PX_GLTF_PTRINDEX(PX_Gltf_AnimationSampler, PX_Gltf_JsonToInt(tokens + i, json_chunk));
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "target") == 0)
		{
			++i;

			PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

			px_int target_size = tokens[i].size;
			++i;

			for (px_int k = 0; k < target_size; ++k)
			{
				PX_GLTF_CHECK_KEY(tokens[i]);

				if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "node") == 0)
				{
					++i;
					out_channel->target_node = PX_GLTF_PTRINDEX(PX_Gltf_Node, PX_Gltf_JsonToInt(tokens + i, json_chunk));
					++i;
				}
				else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "path") == 0)
				{
					++i;
					if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "translation") == 0)
					{
						out_channel->target_path = PX_GLTF_ANIMATION_PATH_TYPE_TRANSLATION;
					}
					else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "rotation") == 0)
					{
						out_channel->target_path = PX_GLTF_ANIMATION_PATH_TYPE_ROTATION;
					}
					else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "scale") == 0)
					{
						out_channel->target_path = PX_GLTF_ANIMATION_PATH_TYPE_SCALE;
					}
					else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "weights") == 0)
					{
						out_channel->target_path = PX_GLTF_ANIMATION_PATH_TYPE_WEIGHTS;
					}
					++i;
				}
				else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
				{
					i = PX_Gltf_ParseJsonExtras(options, tokens, i + 1, json_chunk, &out_channel->extras);
				}
				else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extensions") == 0)
				{
					i = PX_Gltf_ParseJsonUnprocessedExtensions(options, tokens, i, json_chunk, &out_channel->extensions_count, &out_channel->extensions);
				}
				else
				{
					i = PX_Gltf_SkipJson(tokens, i+1);
				}

				if (i < 0)
				{
					return i;
				}
			}
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonAnimation(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Animation* out_animation)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "name") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_animation->name);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "samplers") == 0)
		{
			i = PX_Gltf_ParseJsonArray(options, tokens, i + 1, json_chunk, sizeof(PX_Gltf_AnimationSampler), (px_void**)&out_animation->samplers, &out_animation->samplers_count);
			if (i < 0)
			{
				return i;
			}

			for (px_gltf_size k = 0; k < out_animation->samplers_count; ++k)
			{
				i = PX_Gltf_ParseJsonAnimationSampler(options, tokens, i, json_chunk, &out_animation->samplers[k]);
				if (i < 0)
				{
					return i;
				}
			}
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "channels") == 0)
		{
			i = PX_Gltf_ParseJsonArray(options, tokens, i + 1, json_chunk, sizeof(PX_Gltf_AnimationChannel), (px_void**)&out_animation->channels, &out_animation->channels_count);
			if (i < 0)
			{
				return i;
			}

			for (px_gltf_size k = 0; k < out_animation->channels_count; ++k)
			{
				i = PX_Gltf_ParseJsonAnimationChannel(options, tokens, i, json_chunk, &out_animation->channels[k]);
				if (i < 0)
				{
					return i;
				}
			}
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
		{
			i = PX_Gltf_ParseJsonExtras(options, tokens, i + 1, json_chunk, &out_animation->extras);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extensions") == 0)
		{
			i = PX_Gltf_ParseJsonUnprocessedExtensions(options, tokens, i, json_chunk, &out_animation->extensions_count, &out_animation->extensions);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonAnimations(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Data* out_data)
{
	i = PX_Gltf_ParseJsonArray(options, tokens, i, json_chunk, sizeof(PX_Gltf_Animation), (px_void**)&out_data->animations, &out_data->animations_count);
	if (i < 0)
	{
		return i;
	}

	for (px_gltf_size j = 0; j < out_data->animations_count; ++j)
	{
		i = PX_Gltf_ParseJsonAnimation(options, tokens, i, json_chunk, &out_data->animations[j]);
		if (i < 0)
		{
			return i;
		}
	}
	return i;
}

static px_int PX_Gltf_ParseJsonVariant(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_MaterialVariant* out_variant)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "name") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_variant->name);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
		{
			i = PX_Gltf_ParseJsonExtras(options, tokens, i + 1, json_chunk, &out_variant->extras);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

static px_int PX_Gltf_ParseJsonVariants(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Data* out_data)
{
	i = PX_Gltf_ParseJsonArray(options, tokens, i, json_chunk, sizeof(PX_Gltf_MaterialVariant), (px_void**)&out_data->variants, &out_data->variants_count);
	if (i < 0)
	{
		return i;
	}

	for (px_gltf_size j = 0; j < out_data->variants_count; ++j)
	{
		i = PX_Gltf_ParseJsonVariant(options, tokens, i, json_chunk, &out_data->variants[j]);
		if (i < 0)
		{
			return i;
		}
	}
	return i;
}

static px_int PX_Gltf_ParseJsonAsset(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Asset* out_asset)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "copyright") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_asset->copyright);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "generator") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_asset->generator);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "version") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_asset->version);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "minVersion") == 0)
		{
			i = PX_Gltf_ParseJsonString(options, tokens, i + 1, json_chunk, &out_asset->min_version);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extras") == 0)
		{
			i = PX_Gltf_ParseJsonExtras(options, tokens, i + 1, json_chunk, &out_asset->extras);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extensions") == 0)
		{
			i = PX_Gltf_ParseJsonUnprocessedExtensions(options, tokens, i, json_chunk, &out_asset->extensions_count, &out_asset->extensions);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i+1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	if (out_asset->version && PX_Gltf_StringToDouble(out_asset->version, PX_strlen(out_asset->version)) < 2)
	{
		return PX_GLTF_ERROR_LEGACY;
	}

	return i;
}

px_gltf_size PX_GltfNumComponents(PX_GLTF_TYPE type) {
	switch (type)
	{
	case PX_GLTF_TYPE_VEC2:
		return 2;
	case PX_GLTF_TYPE_VEC3:
		return 3;
	case PX_GLTF_TYPE_VEC4:
		return 4;
	case PX_GLTF_TYPE_MAT2:
		return 4;
	case PX_GLTF_TYPE_MAT3:
		return 9;
	case PX_GLTF_TYPE_MAT4:
		return 16;
	case PX_GLTF_TYPE_INVALID:
	case PX_GLTF_TYPE_SCALAR:
	default:
		return 1;
	}
}

px_gltf_size PX_GltfComponentSize(PX_GLTF_COMPONENT_TYPE component_type) {
	switch (component_type)
	{
	case PX_GLTF_COMPONENT_TYPE_R_8:
	case PX_GLTF_COMPONENT_TYPE_R_8U:
		return 1;
	case PX_GLTF_COMPONENT_TYPE_R_16:
	case PX_GLTF_COMPONENT_TYPE_R_16U:
		return 2;
	case PX_GLTF_COMPONENT_TYPE_R_32U:
	case PX_GLTF_COMPONENT_TYPE_R_32F:
		return 4;
	case PX_GLTF_COMPONENT_TYPE_INVALID:
	default:
		return 0;
	}
}

px_gltf_size PX_GltfCalcSize(PX_GLTF_TYPE type, PX_GLTF_COMPONENT_TYPE component_type)
{
	px_gltf_size component_size = PX_GltfComponentSize(component_type);
	if (type == PX_GLTF_TYPE_MAT2 && component_size == 1)
	{
		return 8 * component_size;
	}
	else if (type == PX_GLTF_TYPE_MAT3 && (component_size == 1 || component_size == 2))
	{
		return 12 * component_size;
	}
	return component_size * PX_GltfNumComponents(type);
}

static px_int PX_Gltf_FixupPointers(PX_Gltf_Data* out_data);

static px_int PX_Gltf_ParseJsonRoot(PX_Gltf_Options* options, PX_Gltf_JsmnToken const* tokens, px_int i, const px_byte* json_chunk, PX_Gltf_Data* out_data)
{
	PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

	px_int size = tokens[i].size;
	++i;

	for (px_int j = 0; j < size; ++j)
	{
		PX_GLTF_CHECK_KEY(tokens[i]);

		if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "asset") == 0)
		{
			i = PX_Gltf_ParseJsonAsset(options, tokens, i + 1, json_chunk, &out_data->asset);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "meshes") == 0)
		{
			i = PX_Gltf_ParseJsonMeshes(options, tokens, i + 1, json_chunk, out_data);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "accessors") == 0)
		{
			i = PX_Gltf_ParseJsonAccessors(options, tokens, i + 1, json_chunk, out_data);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "bufferViews") == 0)
		{
			i = PX_Gltf_ParseJsonBufferViews(options, tokens, i + 1, json_chunk, out_data);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "buffers") == 0)
		{
			i = PX_Gltf_ParseJsonBuffers(options, tokens, i + 1, json_chunk, out_data);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "materials") == 0)
		{
			i = PX_Gltf_ParseJsonMaterials(options, tokens, i + 1, json_chunk, out_data);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "images") == 0)
		{
			i = PX_Gltf_ParseJsonImages(options, tokens, i + 1, json_chunk, out_data);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "textures") == 0)
		{
			i = PX_Gltf_ParseJsonTextures(options, tokens, i + 1, json_chunk, out_data);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "samplers") == 0)
		{
			i = PX_Gltf_ParseJsonSamplers(options, tokens, i + 1, json_chunk, out_data);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "skins") == 0)
		{
			i = PX_Gltf_ParseJsonSkins(options, tokens, i + 1, json_chunk, out_data);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "cameras") == 0)
		{
			i = PX_Gltf_ParseJsonCameras(options, tokens, i + 1, json_chunk, out_data);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "nodes") == 0)
		{
			i = PX_Gltf_ParseJsonNodes(options, tokens, i + 1, json_chunk, out_data);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "scenes") == 0)
		{
			i = PX_Gltf_ParseJsonScenes(options, tokens, i + 1, json_chunk, out_data);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "scene") == 0)
		{
			++i;
			out_data->scene = PX_GLTF_PTRINDEX(PX_Gltf_Scene, PX_Gltf_JsonToInt(tokens + i, json_chunk));
			++i;
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "animations") == 0)
		{
			i = PX_Gltf_ParseJsonAnimations(options, tokens, i + 1, json_chunk, out_data);
		}
		else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "extras") == 0)
		{
			i = PX_Gltf_ParseJsonExtras(options, tokens, i + 1, json_chunk, &out_data->extras);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extensions") == 0)
		{
			++i;

			PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);
			if(out_data->data_extensions)
			{
				return PX_GLTF_ERROR_JSON;
			}

			px_int extensions_size = tokens[i].size;
			out_data->data_extensions_count = 0;
			out_data->data_extensions = (PX_Gltf_Extension*)PX_Gltf_Calloc(options, sizeof(PX_Gltf_Extension), extensions_size);

			if (!out_data->data_extensions)
			{
				return PX_GLTF_ERROR_NOMEM;
			}

			++i;

			for (px_int k = 0; k < extensions_size; ++k)
			{
				PX_GLTF_CHECK_KEY(tokens[i]);

				if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "KHR_lights_punctual") == 0)
				{
					++i;

					PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

					px_int data_size = tokens[i].size;
					++i;

					for (px_int m = 0; m < data_size; ++m)
					{
						PX_GLTF_CHECK_KEY(tokens[i]);

						if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "lights") == 0)
						{
							i = PX_Gltf_ParseJsonLights(options, tokens, i + 1, json_chunk, out_data);
						}
						else
						{
							i = PX_Gltf_SkipJson(tokens, i + 1);
						}

						if (i < 0)
						{
							return i;
						}
					}
				}
				else if (PX_Gltf_JsonStrcmp(tokens+i, json_chunk, "KHR_materials_variants") == 0)
				{
					++i;

					PX_GLTF_CHECK_TOKTYPE(tokens[i], PX_GLTF_JSMN_OBJECT);

					px_int data_size = tokens[i].size;
					++i;

					for (px_int m = 0; m < data_size; ++m)
					{
						PX_GLTF_CHECK_KEY(tokens[i]);

						if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "variants") == 0)
						{
							i = PX_Gltf_ParseJsonVariants(options, tokens, i + 1, json_chunk, out_data);
						}
						else
						{
							i = PX_Gltf_SkipJson(tokens, i + 1);
						}

						if (i < 0)
						{
							return i;
						}
					}
				}
				else
				{
					i = PX_Gltf_ParseJsonUnprocessedExtension(options, tokens, i, json_chunk, &(out_data->data_extensions[out_data->data_extensions_count++]));
				}

				if (i < 0)
				{
					return i;
				}
			}
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extensionsUsed") == 0)
		{
			i = PX_Gltf_ParseJsonStringArray(options, tokens, i + 1, json_chunk, &out_data->extensions_used, &out_data->extensions_used_count);
		}
		else if (PX_Gltf_JsonStrcmp(tokens + i, json_chunk, "extensionsRequired") == 0)
		{
			i = PX_Gltf_ParseJsonStringArray(options, tokens, i + 1, json_chunk, &out_data->extensions_required, &out_data->extensions_required_count);
		}
		else
		{
			i = PX_Gltf_SkipJson(tokens, i + 1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

PX_GLTF_RESULT PX_Gltf_ParseJson(PX_Gltf_Options* options, const px_byte* json_chunk, px_gltf_size size, PX_Gltf_Data** out_data)
{
	PX_Gltf_JsmnParser parser = { 0, 0, 0 };

	if (options->json_token_count == 0)
	{
		px_int token_count = PX_Gltf_JsmnParse(&parser, (const px_char*)json_chunk, size, PX_NULL, 0);

		if (token_count <= 0)
		{
			return PX_GLTF_RESULT_INVALID_JSON;
		}

		options->json_token_count = token_count;
	}

	PX_Gltf_JsmnToken* tokens = (PX_Gltf_JsmnToken*)options->memory.alloc_func(options->memory.user_data, sizeof(PX_Gltf_JsmnToken) * (options->json_token_count + 1));

	if (!tokens)
	{
		return PX_GLTF_RESULT_OUT_OF_MEMORY;
	}

	PX_Gltf_JsmnInit(&parser);

	px_int token_count = PX_Gltf_JsmnParse(&parser, (const px_char*)json_chunk, size, tokens, options->json_token_count);

	if (token_count <= 0)
	{
		options->memory.free_func(options->memory.user_data, tokens);
		return PX_GLTF_RESULT_INVALID_JSON;
	}

	// this makes sure that we always have an UNDEFINED token at the end of the stream
	// for invalid JSON inputs this makes sure we don't perform out of bound reads of token data
	tokens[token_count].type = PX_GLTF_JSMN_UNDEFINED;

	PX_Gltf_Data* data = (PX_Gltf_Data*)options->memory.alloc_func(options->memory.user_data, sizeof(PX_Gltf_Data));

	if (!data)
	{
		options->memory.free_func(options->memory.user_data, tokens);
		return PX_GLTF_RESULT_OUT_OF_MEMORY;
	}

	PX_memset(data, 0, (px_int)(sizeof(PX_Gltf_Data)));
	data->memory = options->memory;
	data->file = options->file;

	px_int i = PX_Gltf_ParseJsonRoot(options, tokens, 0, json_chunk, data);

	options->memory.free_func(options->memory.user_data, tokens);

	if (i < 0)
	{
		PX_GltfFree(data);

		switch (i)
		{
		case PX_GLTF_ERROR_NOMEM: return PX_GLTF_RESULT_OUT_OF_MEMORY;
		case PX_GLTF_ERROR_LEGACY: return PX_GLTF_RESULT_LEGACY_GLTF;
		default: return PX_GLTF_RESULT_INVALID_GLTF;
		}
	}

	if (PX_Gltf_FixupPointers(data) < 0)
	{
		PX_GltfFree(data);
		return PX_GLTF_RESULT_INVALID_GLTF;
	}

	data->json = (const px_char*)json_chunk;
	data->json_size = size;

	*out_data = data;

	return PX_GLTF_RESULT_SUCCESS;
}

static px_int PX_Gltf_FixupPointers(PX_Gltf_Data* data)
{
	for (px_gltf_size i = 0; i < data->meshes_count; ++i)
	{
		for (px_gltf_size j = 0; j < data->meshes[i].primitives_count; ++j)
		{
			PX_GLTF_PTRFIXUP(data->meshes[i].primitives[j].indices, data->accessors, data->accessors_count);
			PX_GLTF_PTRFIXUP(data->meshes[i].primitives[j].material, data->materials, data->materials_count);

			for (px_gltf_size k = 0; k < data->meshes[i].primitives[j].attributes_count; ++k)
			{
				PX_GLTF_PTRFIXUP_REQ(data->meshes[i].primitives[j].attributes[k].data, data->accessors, data->accessors_count);
			}

			for (px_gltf_size k = 0; k < data->meshes[i].primitives[j].targets_count; ++k)
			{
				for (px_gltf_size m = 0; m < data->meshes[i].primitives[j].targets[k].attributes_count; ++m)
				{
					PX_GLTF_PTRFIXUP_REQ(data->meshes[i].primitives[j].targets[k].attributes[m].data, data->accessors, data->accessors_count);
				}
			}

			if (data->meshes[i].primitives[j].has_draco_mesh_compression)
			{
				PX_GLTF_PTRFIXUP_REQ(data->meshes[i].primitives[j].draco_mesh_compression.buffer_view, data->buffer_views, data->buffer_views_count);
				for (px_gltf_size m = 0; m < data->meshes[i].primitives[j].draco_mesh_compression.attributes_count; ++m)
				{
					PX_GLTF_PTRFIXUP_REQ(data->meshes[i].primitives[j].draco_mesh_compression.attributes[m].data, data->accessors, data->accessors_count);
				}
			}

			for (px_gltf_size k = 0; k < data->meshes[i].primitives[j].mappings_count; ++k)
			{
				PX_GLTF_PTRFIXUP_REQ(data->meshes[i].primitives[j].mappings[k].material, data->materials, data->materials_count);
			}
		}
	}

	for (px_gltf_size i = 0; i < data->accessors_count; ++i)
	{
		PX_GLTF_PTRFIXUP(data->accessors[i].buffer_view, data->buffer_views, data->buffer_views_count);

		if (data->accessors[i].is_sparse)
		{
			PX_GLTF_PTRFIXUP_REQ(data->accessors[i].sparse.indices_buffer_view, data->buffer_views, data->buffer_views_count);
			PX_GLTF_PTRFIXUP_REQ(data->accessors[i].sparse.values_buffer_view, data->buffer_views, data->buffer_views_count);
		}

		if (data->accessors[i].buffer_view)
		{
			data->accessors[i].stride = data->accessors[i].buffer_view->stride;
		}

		if (data->accessors[i].stride == 0)
		{
			data->accessors[i].stride = PX_GltfCalcSize(data->accessors[i].type, data->accessors[i].component_type);
		}
	}

	for (px_gltf_size i = 0; i < data->textures_count; ++i)
	{
		PX_GLTF_PTRFIXUP(data->textures[i].image, data->images, data->images_count);
		PX_GLTF_PTRFIXUP(data->textures[i].basisu_image, data->images, data->images_count);
		PX_GLTF_PTRFIXUP(data->textures[i].webp_image, data->images, data->images_count);
		PX_GLTF_PTRFIXUP(data->textures[i].sampler, data->samplers, data->samplers_count);
	}

	for (px_gltf_size i = 0; i < data->images_count; ++i)
	{
		PX_GLTF_PTRFIXUP(data->images[i].buffer_view, data->buffer_views, data->buffer_views_count);
	}

	for (px_gltf_size i = 0; i < data->materials_count; ++i)
	{
		PX_GLTF_PTRFIXUP(data->materials[i].normal_texture.texture, data->textures, data->textures_count);
		PX_GLTF_PTRFIXUP(data->materials[i].emissive_texture.texture, data->textures, data->textures_count);
		PX_GLTF_PTRFIXUP(data->materials[i].occlusion_texture.texture, data->textures, data->textures_count);

		PX_GLTF_PTRFIXUP(data->materials[i].pbr_metallic_roughness.base_color_texture.texture, data->textures, data->textures_count);
		PX_GLTF_PTRFIXUP(data->materials[i].pbr_metallic_roughness.metallic_roughness_texture.texture, data->textures, data->textures_count);

		PX_GLTF_PTRFIXUP(data->materials[i].pbr_specular_glossiness.diffuse_texture.texture, data->textures, data->textures_count);
		PX_GLTF_PTRFIXUP(data->materials[i].pbr_specular_glossiness.specular_glossiness_texture.texture, data->textures, data->textures_count);

		PX_GLTF_PTRFIXUP(data->materials[i].clearcoat.clearcoat_texture.texture, data->textures, data->textures_count);
		PX_GLTF_PTRFIXUP(data->materials[i].clearcoat.clearcoat_roughness_texture.texture, data->textures, data->textures_count);
		PX_GLTF_PTRFIXUP(data->materials[i].clearcoat.clearcoat_normal_texture.texture, data->textures, data->textures_count);

		PX_GLTF_PTRFIXUP(data->materials[i].specular.specular_texture.texture, data->textures, data->textures_count);
		PX_GLTF_PTRFIXUP(data->materials[i].specular.specular_color_texture.texture, data->textures, data->textures_count);

		PX_GLTF_PTRFIXUP(data->materials[i].transmission.transmission_texture.texture, data->textures, data->textures_count);

		PX_GLTF_PTRFIXUP(data->materials[i].volume.thickness_texture.texture, data->textures, data->textures_count);

		PX_GLTF_PTRFIXUP(data->materials[i].sheen.sheen_color_texture.texture, data->textures, data->textures_count);
		PX_GLTF_PTRFIXUP(data->materials[i].sheen.sheen_roughness_texture.texture, data->textures, data->textures_count);

		PX_GLTF_PTRFIXUP(data->materials[i].iridescence.iridescence_texture.texture, data->textures, data->textures_count);
		PX_GLTF_PTRFIXUP(data->materials[i].iridescence.iridescence_thickness_texture.texture, data->textures, data->textures_count);

		PX_GLTF_PTRFIXUP(data->materials[i].diffuse_transmission.diffuse_transmission_texture.texture, data->textures, data->textures_count);
		PX_GLTF_PTRFIXUP(data->materials[i].diffuse_transmission.diffuse_transmission_color_texture.texture, data->textures, data->textures_count);

		PX_GLTF_PTRFIXUP(data->materials[i].anisotropy.anisotropy_texture.texture, data->textures, data->textures_count);
	}

	for (px_gltf_size i = 0; i < data->buffer_views_count; ++i)
	{
		PX_GLTF_PTRFIXUP_REQ(data->buffer_views[i].buffer, data->buffers, data->buffers_count);

		if (data->buffer_views[i].has_meshopt_compression)
		{
			PX_GLTF_PTRFIXUP_REQ(data->buffer_views[i].meshopt_compression.buffer, data->buffers, data->buffers_count);
		}
	}

	for (px_gltf_size i = 0; i < data->skins_count; ++i)
	{
		for (px_gltf_size j = 0; j < data->skins[i].joints_count; ++j)
		{
			PX_GLTF_PTRFIXUP_REQ(data->skins[i].joints[j], data->nodes, data->nodes_count);
		}

		PX_GLTF_PTRFIXUP(data->skins[i].skeleton, data->nodes, data->nodes_count);
		PX_GLTF_PTRFIXUP(data->skins[i].inverse_bind_matrices, data->accessors, data->accessors_count);
	}

	for (px_gltf_size i = 0; i < data->nodes_count; ++i)
	{
		for (px_gltf_size j = 0; j < data->nodes[i].children_count; ++j)
		{
			PX_GLTF_PTRFIXUP_REQ(data->nodes[i].children[j], data->nodes, data->nodes_count);

			if (data->nodes[i].children[j]->parent)
			{
				return PX_GLTF_ERROR_JSON;
			}

			data->nodes[i].children[j]->parent = &data->nodes[i];
		}

		PX_GLTF_PTRFIXUP(data->nodes[i].mesh, data->meshes, data->meshes_count);
		PX_GLTF_PTRFIXUP(data->nodes[i].skin, data->skins, data->skins_count);
		PX_GLTF_PTRFIXUP(data->nodes[i].camera, data->cameras, data->cameras_count);
		PX_GLTF_PTRFIXUP(data->nodes[i].light, data->lights, data->lights_count);

		if (data->nodes[i].has_mesh_gpu_instancing)
		{
			for (px_gltf_size m = 0; m < data->nodes[i].mesh_gpu_instancing.attributes_count; ++m)
			{
				PX_GLTF_PTRFIXUP_REQ(data->nodes[i].mesh_gpu_instancing.attributes[m].data, data->accessors, data->accessors_count);
			}
		}
	}

	for (px_gltf_size i = 0; i < data->scenes_count; ++i)
	{
		for (px_gltf_size j = 0; j < data->scenes[i].nodes_count; ++j)
		{
			PX_GLTF_PTRFIXUP_REQ(data->scenes[i].nodes[j], data->nodes, data->nodes_count);

			if (data->scenes[i].nodes[j]->parent)
			{
				return PX_GLTF_ERROR_JSON;
			}
		}
	}

	PX_GLTF_PTRFIXUP(data->scene, data->scenes, data->scenes_count);

	for (px_gltf_size i = 0; i < data->animations_count; ++i)
	{
		for (px_gltf_size j = 0; j < data->animations[i].samplers_count; ++j)
		{
			PX_GLTF_PTRFIXUP_REQ(data->animations[i].samplers[j].input, data->accessors, data->accessors_count);
			PX_GLTF_PTRFIXUP_REQ(data->animations[i].samplers[j].output, data->accessors, data->accessors_count);
		}

		for (px_gltf_size j = 0; j < data->animations[i].channels_count; ++j)
		{
			PX_GLTF_PTRFIXUP_REQ(data->animations[i].channels[j].sampler, data->animations[i].samplers, data->animations[i].samplers_count);
			PX_GLTF_PTRFIXUP(data->animations[i].channels[j].target_node, data->nodes, data->nodes_count);
		}
	}

	return 0;
}

/*
 * -- jsmn.c start --
 * Source: https://github.com/zserge/jsmn
 * License: MIT
 *
 * Copyright (c) 2010 Serge A. Zaitsev

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * Allocates a fresh unused token from the token pull.
 */
static PX_Gltf_JsmnToken *PX_Gltf_JsmnAllocToken(PX_Gltf_JsmnParser *parser,
				   PX_Gltf_JsmnToken *tokens, px_gltf_size num_tokens) {
	PX_Gltf_JsmnToken *tok;
	if (parser->toknext >= num_tokens) {
		return PX_NULL;
	}
	tok = &tokens[parser->toknext++];
	tok->start = tok->end = -1;
	tok->size = 0;
#ifdef PX_GLTF_JSMN_PARENT_LINKS
	tok->parent = -1;
#endif
	return tok;
}

/**
 * Fills token type and boundaries.
 */
static px_void PX_Gltf_JsmnFillToken(PX_Gltf_JsmnToken *token, PX_GLTF_JSMN_TYPE type,
				px_gltf_ssize start, px_gltf_ssize end) {
	token->type = type;
	token->start = start;
	token->end = end;
	token->size = 0;
}

/**
 * Fills next available token with JSON primitive.
 */
static px_int PX_Gltf_JsmnParsePrimitive(PX_Gltf_JsmnParser *parser, const px_char *js,
				px_gltf_size len, PX_Gltf_JsmnToken *tokens, px_gltf_size num_tokens) {
	PX_Gltf_JsmnToken *token;
	px_gltf_ssize start;

	start = parser->pos;

	for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
		switch (js[parser->pos]) {
#ifndef PX_GLTF_JSMN_STRICT
		/* In strict mode primitive must be followed by "," or "}" or "]" */
		case ':':
#endif
		case '\t' : case '\r' : case '\n' : case ' ' :
		case ','  : case ']'  : case '}' :
			goto found;
		}
		if (js[parser->pos] < 32 || js[parser->pos] >= 127) {
			parser->pos = start;
			return PX_GLTF_JSMN_ERROR_INVAL;
		}
	}
#ifdef PX_GLTF_JSMN_STRICT
	/* In strict mode primitive must be followed by a comma/object/array */
	parser->pos = start;
	return PX_GLTF_JSMN_ERROR_PART;
#endif

found:
	if (tokens == PX_NULL) {
		parser->pos--;
		return 0;
	}
	token = PX_Gltf_JsmnAllocToken(parser, tokens, num_tokens);
	if (token == PX_NULL) {
		parser->pos = start;
		return PX_GLTF_JSMN_ERROR_NOMEM;
	}
	PX_Gltf_JsmnFillToken(token, PX_GLTF_JSMN_PRIMITIVE, start, parser->pos);
#ifdef PX_GLTF_JSMN_PARENT_LINKS
	token->parent = parser->toksuper;
#endif
	parser->pos--;
	return 0;
}

/**
 * Fills next token with JSON string.
 */
static px_int PX_Gltf_JsmnParseString(PX_Gltf_JsmnParser *parser, const px_char *js,
				 px_gltf_size len, PX_Gltf_JsmnToken *tokens, px_gltf_size num_tokens) {
	PX_Gltf_JsmnToken *token;

	px_gltf_ssize start = parser->pos;

	parser->pos++;

	/* Skip starting quote */
	for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
		px_char c = js[parser->pos];

		/* Quote: end of string */
		if (c == '\"') {
			if (tokens == PX_NULL) {
				return 0;
			}
			token = PX_Gltf_JsmnAllocToken(parser, tokens, num_tokens);
			if (token == PX_NULL) {
				parser->pos = start;
				return PX_GLTF_JSMN_ERROR_NOMEM;
			}
			PX_Gltf_JsmnFillToken(token, PX_GLTF_JSMN_STRING, start+1, parser->pos);
#ifdef PX_GLTF_JSMN_PARENT_LINKS
			token->parent = parser->toksuper;
#endif
			return 0;
		}

		/* Backslash: Quoted symbol expected */
		if (c == '\\' && parser->pos + 1 < len) {
			px_int i;
			parser->pos++;
			switch (js[parser->pos]) {
			/* Allowed escaped symbols */
			case '\"': case '/' : case '\\' : case 'b' :
			case 'f' : case 'r' : case 'n'  : case 't' :
				break;
				/* Allows escaped symbol \uXXXX */
			case 'u':
				parser->pos++;
				for(i = 0; i < 4 && parser->pos < len && js[parser->pos] != '\0'; i++) {
					/* If it isn't a hex character we have an error */
					if(!((js[parser->pos] >= 48 && js[parser->pos] <= 57) || /* 0-9 */
						 (js[parser->pos] >= 65 && js[parser->pos] <= 70) || /* A-F */
						 (js[parser->pos] >= 97 && js[parser->pos] <= 102))) { /* a-f */
						parser->pos = start;
						return PX_GLTF_JSMN_ERROR_INVAL;
					}
					parser->pos++;
				}
				parser->pos--;
				break;
				/* Unexpected symbol */
			default:
				parser->pos = start;
				return PX_GLTF_JSMN_ERROR_INVAL;
			}
		}
	}
	parser->pos = start;
	return PX_GLTF_JSMN_ERROR_PART;
}

/**
 * Parse JSON string and fill tokens.
 */
static px_int PX_Gltf_JsmnParse(PX_Gltf_JsmnParser *parser, const px_char *js, px_gltf_size len,
		   PX_Gltf_JsmnToken *tokens, px_gltf_size num_tokens) {
	px_int r;
	px_int i;
	PX_Gltf_JsmnToken *token;
	px_int count = parser->toknext;

	for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
		px_char c;
		PX_GLTF_JSMN_TYPE type;

		c = js[parser->pos];
		switch (c) {
		case '{': case '[':
			count++;
			if (tokens == PX_NULL) {
				break;
			}
			token = PX_Gltf_JsmnAllocToken(parser, tokens, num_tokens);
			if (token == PX_NULL)
				return PX_GLTF_JSMN_ERROR_NOMEM;
			if (parser->toksuper != -1) {
				tokens[parser->toksuper].size++;
#ifdef PX_GLTF_JSMN_PARENT_LINKS
				token->parent = parser->toksuper;
#endif
			}
			token->type = (c == '{' ? PX_GLTF_JSMN_OBJECT : PX_GLTF_JSMN_ARRAY);
			token->start = parser->pos;
			parser->toksuper = parser->toknext - 1;
			break;
		case '}': case ']':
			if (tokens == PX_NULL)
				break;
			type = (c == '}' ? PX_GLTF_JSMN_OBJECT : PX_GLTF_JSMN_ARRAY);
#ifdef PX_GLTF_JSMN_PARENT_LINKS
			if (parser->toknext < 1) {
				return PX_GLTF_JSMN_ERROR_INVAL;
			}
			token = &tokens[parser->toknext - 1];
			for (;;) {
				if (token->start != -1 && token->end == -1) {
					if (token->type != type) {
						return PX_GLTF_JSMN_ERROR_INVAL;
					}
					token->end = parser->pos + 1;
					parser->toksuper = token->parent;
					break;
				}
				if (token->parent == -1) {
					if(token->type != type || parser->toksuper == -1) {
						return PX_GLTF_JSMN_ERROR_INVAL;
					}
					break;
				}
				token = &tokens[token->parent];
			}
#else
			for (i = parser->toknext - 1; i >= 0; i--) {
				token = &tokens[i];
				if (token->start != -1 && token->end == -1) {
					if (token->type != type) {
						return PX_GLTF_JSMN_ERROR_INVAL;
					}
					parser->toksuper = -1;
					token->end = parser->pos + 1;
					break;
				}
			}
			/* Error if unmatched closing bracket */
			if (i == -1) return PX_GLTF_JSMN_ERROR_INVAL;
			for (; i >= 0; i--) {
				token = &tokens[i];
				if (token->start != -1 && token->end == -1) {
					parser->toksuper = i;
					break;
				}
			}
#endif
			break;
		case '\"':
			r = PX_Gltf_JsmnParseString(parser, js, len, tokens, num_tokens);
			if (r < 0) return r;
			count++;
			if (parser->toksuper != -1 && tokens != PX_NULL)
				tokens[parser->toksuper].size++;
			break;
		case '\t' : case '\r' : case '\n' : case ' ':
			break;
		case ':':
			parser->toksuper = parser->toknext - 1;
			break;
		case ',':
			if (tokens != PX_NULL && parser->toksuper != -1 &&
					tokens[parser->toksuper].type != PX_GLTF_JSMN_ARRAY &&
					tokens[parser->toksuper].type != PX_GLTF_JSMN_OBJECT) {
#ifdef PX_GLTF_JSMN_PARENT_LINKS
				parser->toksuper = tokens[parser->toksuper].parent;
#else
				for (i = parser->toknext - 1; i >= 0; i--) {
					if (tokens[i].type == PX_GLTF_JSMN_ARRAY || tokens[i].type == PX_GLTF_JSMN_OBJECT) {
						if (tokens[i].start != -1 && tokens[i].end == -1) {
							parser->toksuper = i;
							break;
						}
					}
				}
#endif
			}
			break;
#ifdef PX_GLTF_JSMN_STRICT
			/* In strict mode primitives are: numbers and booleans */
		case '-': case '0': case '1' : case '2': case '3' : case '4':
		case '5': case '6': case '7' : case '8': case '9':
		case 't': case 'f': case 'n' :
			/* And they must not be keys of the object */
			if (tokens != PX_NULL && parser->toksuper != -1) {
				PX_Gltf_JsmnToken *t = &tokens[parser->toksuper];
				if (t->type == PX_GLTF_JSMN_OBJECT ||
						(t->type == PX_GLTF_JSMN_STRING && t->size != 0)) {
					return PX_GLTF_JSMN_ERROR_INVAL;
				}
			}
#else
			/* In non-strict mode every unquoted value is a primitive */
		default:
#endif
			r = PX_Gltf_JsmnParsePrimitive(parser, js, len, tokens, num_tokens);
			if (r < 0) return r;
			count++;
			if (parser->toksuper != -1 && tokens != PX_NULL)
				tokens[parser->toksuper].size++;
			break;

#ifdef PX_GLTF_JSMN_STRICT
			/* Unexpected px_char in strict mode */
		default:
			return PX_GLTF_JSMN_ERROR_INVAL;
#endif
		}
	}

	if (tokens != PX_NULL) {
		for (i = parser->toknext - 1; i >= 0; i--) {
			/* Unmatched opened object or array */
			if (tokens[i].start != -1 && tokens[i].end == -1) {
				return PX_GLTF_JSMN_ERROR_PART;
			}
		}
	}

	return count;
}

/**
 * Creates a new parser based over a given  buffer with an array of tokens
 * available.
 */
static px_void PX_Gltf_JsmnInit(PX_Gltf_JsmnParser *parser) {
	parser->pos = 0;
	parser->toknext = 0;
	parser->toksuper = -1;
}
/*
 * -- jsmn.c end --
 */

