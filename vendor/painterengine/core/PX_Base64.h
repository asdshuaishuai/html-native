#ifndef PX_BASE64_H
#define PX_BASE64_H
#include "PX_Typedef.h"
px_uint PX_Base64Encode(const px_byte *_in, px_uint input_size, px_char *out);
px_uint PX_Base64Decode(const px_char *_in, px_uint input_size, px_byte *out);
px_uint PX_Base64GetEncodeBytesLength(px_uint codeLen);
px_uint PX_Base64GetDecodeBytesLength(px_uint codeLen);
px_bool PX_Base64Check(const px_char* code, px_uint codeLen);
#endif
