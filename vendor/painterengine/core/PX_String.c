#include "PX_String.h"

px_int PX_StringToInteger(px_string *str)
{
	if (str->buffer[0]=='0'&&str->buffer[1]!='\0'&&(str->buffer[1]=='x'||str->buffer[1]=='X'))
	{
		return PX_htoi(str->buffer+2);
	}
	else
		return PX_atoi(str->buffer);
}

px_void PX_StringTrim(px_string *str)
{
	if (str->buffer)
	{
		PX_trim(str->buffer);
	}
	str->exreg_strlen = PX_strlen(str->buffer);
}

px_void PX_StringTrimLeft(px_string *str,px_int leftCount)
{
	px_int i;
	if (leftCount==0)
	{
		return;
	}
	if (leftCount>=str->exreg_strlen)
	{
		PX_StringClear(str);
		str->exreg_strlen = 0;
		return;
	}
	else
	{
		for (i=0;str->buffer[i+leftCount];i++)
		{
			str->buffer[i]=str->buffer[i+leftCount];
		}
		str->buffer[i]='\0';
		str->exreg_strlen -= leftCount;
	}
	
}

px_void PX_StringTrimRight(px_string *str,px_int RightCount)
{
	if (RightCount==0)
	{
		return;
	}
	if (RightCount>=str->exreg_strlen)
	{
		PX_StringClear(str);
		str->exreg_strlen = 0;
		return;
	}
	else
	{
		str->buffer[str->exreg_strlen-RightCount]='\0';
		str->exreg_strlen -= RightCount;
	}
}
px_void PX_StringTrimBackwardUntil(px_string *str,px_char until_char)
{
	px_int i;
	for (i=str->exreg_strlen-1;i>=0;i--)
	{
		if (str->buffer[i]==until_char)
		{
			str->buffer[i]='\0';
			str->exreg_strlen = i;
			return;
		}
	}
	PX_StringClear(str);
	str->exreg_strlen = 0;
}

px_void PX_StringTrimForwardUntil(px_string *str,px_char until_char)
{
	px_int i;
	for (i=0;str->buffer[i];i++)
	{
		if (str->buffer[i]==until_char)
		{
			PX_StringTrimLeft(str, i+1);
			return;
		}
	}
	PX_StringClear(str);
	str->exreg_strlen = 0;
}

const px_char* PX_StringGetText(px_string* str)
{
	PX_ASSERTIFX(str->buffer == PX_NULL, "PX_StringGetText: str->buffer is NULL"); 
	return PX_STRING_DATA(str);
}

px_char* PX_StringBeginRWBuffer(px_string* str)
{
	return str->buffer;
}

px_void PX_StringEndRWBuffer(px_string* str)
{
	str->exreg_strlen = PX_strlen(str->buffer);
}

px_void PX_StringSubi(px_string* str, px_char delimiters[], px_int subi)
{
	px_int begin,end;
	px_int dsize;

	if (!str||!str->buffer||!delimiters||subi<0)
	{
		return;
	}
	dsize = PX_strlen(delimiters);
	if (dsize<=0)
	{
		return;
	}

	begin = 0;
	while (subi)
	{
		if (begin + dsize > str->exreg_strlen)
		{
			//fewer sub-strings than requested
			PX_StringClear(str);
			return;
		}
		if (PX_memequ(&str->buffer[begin], delimiters, dsize))
		{
			subi--;
			begin += dsize;
		}
		else
		{
			begin++;
		}
	}
	//locate the end of this sub-string
	for (end = begin; end < str->exreg_strlen; end++)
	{
		if (end + dsize <= str->exreg_strlen && PX_memequ(&str->buffer[end], delimiters, dsize))
		{
			break;
		}
	}
	str->buffer[end] = '\0';
	str->exreg_strlen = end;
	PX_StringTrimLeft(str, begin);
	PX_StringUpdateExReg(str);
}

px_bool PX_StringSetSubi(px_string* str, px_char delimiters[], px_int subi, const px_char* psub_text)
{
	px_int begin, end, delimiter_size;

	if (!str || !str->buffer || !delimiters || !psub_text || subi < 0)
	{
		return PX_TRUE;
	}

	delimiter_size = PX_strlen(delimiters);
	if (delimiter_size <= 0)
	{
		return PX_TRUE;
	}

	begin = 0;
	while (subi > 0)
	{
		if (begin + delimiter_size > str->exreg_strlen)
		{
			return PX_TRUE;
		}
		if (PX_memequ(str->buffer + begin, delimiters, delimiter_size))
		{
			begin += delimiter_size;
			subi--;
		}
		else
		{
			begin++;
		}
	}

	if (begin > str->exreg_strlen)
	{
		return PX_TRUE;
	}

	end = begin;
	while (end + delimiter_size <= str->exreg_strlen &&
		!PX_memequ(str->buffer + end, delimiters, delimiter_size))
	{
		end++;
	}

	if (end + delimiter_size > str->exreg_strlen)
	{
		end = str->exreg_strlen;
	}
	/* A valid non-empty substring must have a character range to replace. */
	if (begin >= end)
	{
		return PX_TRUE;
	}

	return PX_StringReplaceRange(str, begin, end - 1, psub_text);
}

px_void PX_StringTrimLeftSubn(px_string* str, px_char delimiters[], px_int subn)
{
	px_int delimiter_size;
	px_int begin;
	px_int delimiter_count;
	px_int length;

	if (!str || !str->buffer || !delimiters || subn <= 0)
	{
		return;
	}

	delimiter_size = PX_strlen(delimiters);
	if (delimiter_size <= 0)
	{
		return;
	}

	PX_StringUpdateExReg(str);
	length = str->exreg_strlen;
	begin = 0;
	delimiter_count = 0;
	while (begin + delimiter_size <= length && delimiter_count < subn)
	{
		if (PX_memequ(str->buffer + begin, delimiters, delimiter_size))
		{
			begin += delimiter_size;
			delimiter_count++;
		}
		else
		{
			begin++;
		}
	}

	if (delimiter_count < subn)
	{
		PX_StringClear(str);
		return;
	}

	PX_memmove(str->buffer, str->buffer + begin, length - begin + 1);
	str->exreg_strlen = length - begin;
}

px_void PX_StringTrimRigthSubn(px_string* str, px_char delimiters[], px_int subn)
{
	px_int delimiter_size;
	px_int end;
	px_int delimiter_count;
	px_int i;

	if (!str || !str->buffer || !delimiters || subn <= 0)
	{
		return;
	}

	delimiter_size = PX_strlen(delimiters);
	if (delimiter_size <= 0)
	{
		return;
	}

	PX_StringUpdateExReg(str);
	end = str->exreg_strlen;
	delimiter_count = 0;
	while (end >= delimiter_size && delimiter_count < subn)
	{
		i = end - delimiter_size;
		while (i >= 0 && !PX_memequ(str->buffer + i, delimiters, delimiter_size))
		{
			i--;
		}
		if (i < 0)
		{
			PX_StringClear(str);
			return;
		}
		end = i;
		delimiter_count++;
	}

	if (delimiter_count < subn)
	{
		PX_StringClear(str);
		return;
	}

	str->buffer[end] = '\0';
	str->exreg_strlen = end;
}

px_int PX_StringFind(px_string* str, const px_char find[])
{
	if (str->buffer)
	{
		px_char*p= PX_strstr(str->buffer, find);
		if (p)
		{
			return (px_int)(p - str->buffer);
		}
	}
	return -1;
	
}
px_int PX_StringFindCharCount(px_string* str, px_char find)
{
	px_int i,count=0;
	if (str->buffer)
	{
		for (i=0;str->buffer[i];i++)
		{
			if (str->buffer[i]==find)
			{
				count++;
			}
		}
	}
	return count;
}
px_bool PX_StringRead(const px_char content[],  px_int readsize, px_char out[])
{
	if (readsize > PX_strlen(content))
	{
		return PX_FALSE;
	}
	PX_memcpy(out, content, readsize);
	out[readsize] = '\0';
	return PX_TRUE;
}

px_int PX_StringReadUntil(const px_char content[], px_char until,  px_char out[], px_int max_out_size)
{
	px_int i;
	for (i = 0; i < max_out_size -1; i++)
	{
		if (content[i]=='\0')
		{
			return 0;
		}
		if (content[i] == until)
		{
			out[i] = '\0';
			return i+1;
		}
		out[i] = content[i];
	}
	return 0;
}

px_void PX_StringFree(px_string *str)
{
	if(str->mp&&str->buffer&&str->bufferlen)
		MP_Free(str->mp,str->buffer);

	str->buffer=PX_NULL;
	str->bufferlen=0;
	str->exreg_strlen = 0;
}

px_bool PX_StringInitialize(px_memorypool *mp,px_string *str)
{
	str->buffer=(px_char *)MP_Malloc(mp,16);
	if(!str->buffer)return PX_FALSE;
	str->bufferlen=16;
	PX_memset(str->buffer,0,16);
	str->mp=mp;
	str->exreg_strlen = 0;
	return PX_TRUE;
}



px_bool PX_StringFormat8(px_string *str,const px_char fmt[],px_stringformat _1, px_stringformat _2, px_stringformat _3, px_stringformat _4,px_stringformat _5, px_stringformat _6, px_stringformat _7, px_stringformat _8)
{
	px_char *oldptr;
	px_uint finalLen=0;
	px_uchar shl=0;
	
	finalLen=PX_sprintf8(PX_NULL,0,fmt,_1,_2,_3,_4,_5,_6,_7,_8);

	oldptr=str->buffer;

	while ((px_uint)(1<<++shl)<=finalLen);
	str->bufferlen=(1<<shl);
	str->buffer=(px_char *)MP_Malloc(str->mp,str->bufferlen);
	if (!str->buffer)
	{
		str->exreg_strlen = 0;
		return PX_FALSE;
	}
	
	finalLen=PX_sprintf8(str->buffer,str->bufferlen,fmt,_1,_2,_3,_4,_5,_6,_7,_8);

	if(oldptr)
		MP_Free(str->mp,oldptr);

	str->exreg_strlen = finalLen;
	return PX_TRUE;
}
px_bool PX_StringFormat7(px_string *str,const px_char fmt[],px_stringformat _1, px_stringformat _2, px_stringformat _3, px_stringformat _4,px_stringformat _5, px_stringformat _6, px_stringformat _7)
{
	return PX_StringFormat8(str,fmt,_1,_2,_3,_4,_5,_6,_7,PX_STRINGFORMAT_INT(0));
}

px_bool PX_StringFormat6(px_string *str,const px_char fmt[],px_stringformat _1, px_stringformat _2, px_stringformat _3, px_stringformat _4,px_stringformat _5, px_stringformat _6)
{
	return PX_StringFormat8(str,fmt,_1,_2,_3,_4,_5,_6,PX_STRINGFORMAT_INT(0),PX_STRINGFORMAT_INT(0));
}

px_bool PX_StringFormat5(px_string *str,const px_char fmt[],px_stringformat _1, px_stringformat _2, px_stringformat _3, px_stringformat _4,px_stringformat _5)
{
	return PX_StringFormat8(str,fmt,_1,_2,_3,_4,_5,PX_STRINGFORMAT_INT(0),PX_STRINGFORMAT_INT(0),PX_STRINGFORMAT_INT(0));
}

px_bool PX_StringFormat4(px_string *str,const px_char fmt[],px_stringformat _1, px_stringformat _2, px_stringformat _3, px_stringformat _4)
{
	return PX_StringFormat8(str,fmt,_1,_2,_3,_4,PX_STRINGFORMAT_INT(0),PX_STRINGFORMAT_INT(0),PX_STRINGFORMAT_INT(0),PX_STRINGFORMAT_INT(0));
}

px_bool PX_StringFormat3(px_string *str,const px_char fmt[],px_stringformat _1, px_stringformat _2, px_stringformat _3)
{
	return PX_StringFormat8(str,fmt,_1,_2,_3,PX_STRINGFORMAT_INT(0),PX_STRINGFORMAT_INT(0),PX_STRINGFORMAT_INT(0),PX_STRINGFORMAT_INT(0),PX_STRINGFORMAT_INT(0));
}

px_bool PX_StringFormat2(px_string *str,const px_char fmt[],px_stringformat _1, px_stringformat _2)
{
	return PX_StringFormat8(str,fmt,_1,_2,PX_STRINGFORMAT_INT(0),PX_STRINGFORMAT_INT(0),PX_STRINGFORMAT_INT(0),PX_STRINGFORMAT_INT(0),PX_STRINGFORMAT_INT(0),PX_STRINGFORMAT_INT(0));
}

px_bool PX_StringFormat1(px_string *str,const px_char fmt[],px_stringformat _1)
{
	return PX_StringFormat8(str,fmt,_1,PX_STRINGFORMAT_INT(0),PX_STRINGFORMAT_INT(0),PX_STRINGFORMAT_INT(0),PX_STRINGFORMAT_INT(0),PX_STRINGFORMAT_INT(0),PX_STRINGFORMAT_INT(0),PX_STRINGFORMAT_INT(0));
}

px_bool PX_StringSet(px_string *str,const px_char fmt[])
{
	px_char *oldptr;
	px_uint finalLen=0;
	px_uchar shl=0;

	finalLen=PX_strlen(fmt);

	if (finalLen<(px_uint)str->bufferlen)
	{
		PX_strset(str->buffer,fmt);
		str->exreg_strlen = finalLen;
		return PX_TRUE;
	}

	oldptr=str->buffer;

	while ((px_uint)(1<<++shl)<=finalLen);
	str->bufferlen=(1<<shl);
	str->buffer=(px_char *)MP_Malloc(str->mp,str->bufferlen);
	if (!str->buffer)
	{
		return PX_FALSE;
	}
	PX_strset(str->buffer,fmt);

	if(oldptr)
		MP_Free(str->mp,oldptr);

	str->exreg_strlen = finalLen;
	return PX_TRUE;
}

px_void PX_StringSetStatic(px_string* str, const px_char fmt[])
{
	if (str->mp)
	{
		MP_Free(str->mp, str->buffer);
	}
	PX_memset(str, 0, sizeof(px_string));
	str->buffer = (px_char*)fmt;
	str->bufferlen = PX_strlen(fmt) + 1;
	str->exreg_strlen = str->bufferlen - 1;
}

px_bool PX_StringInitializeFormat0(px_memorypool* mp, px_string* str, const px_char fmt[])
{
	if (!PX_StringInitialize(mp, str))
	{
		return PX_FALSE;
	}
	return PX_StringSet(str, fmt);
}

px_bool PX_StringInitializeFormat1(px_memorypool* mp, px_string* str, const px_char fmt[], px_stringformat _1)
{
	if (!PX_StringInitialize(mp, str))
	{
		return PX_FALSE;
	}
	return PX_StringFormat1(str, fmt, _1);
}

px_bool PX_StringInitializeFormat2(px_memorypool* mp, px_string* str, const px_char fmt[], px_stringformat _1, px_stringformat _2)
{
	if (!PX_StringInitialize(mp, str))
	{
		return PX_FALSE;
	}
	return PX_StringFormat2(str, fmt, _1, _2);
}

px_bool PX_StringInitializeFormat3(px_memorypool* mp, px_string* str, const px_char fmt[], px_stringformat _1, px_stringformat _2, px_stringformat _3)
{
	if (!PX_StringInitialize(mp, str))
	{
		return PX_FALSE;
	}
	return PX_StringFormat3(str, fmt, _1, _2, _3);
}

px_bool PX_StringInitializeFormat4(px_memorypool* mp, px_string* str, const px_char fmt[], px_stringformat _1, px_stringformat _2, px_stringformat _3, px_stringformat _4)
{
	if (!PX_StringInitialize(mp, str))
	{
		return PX_FALSE;
	}
	return PX_StringFormat4(str, fmt, _1, _2, _3, _4);
}

px_bool PX_StringInitializeFormat5(px_memorypool* mp, px_string* str, const px_char fmt[], px_stringformat _1, px_stringformat _2, px_stringformat _3, px_stringformat _4, px_stringformat _5)
{
	if (!PX_StringInitialize(mp, str))
	{
		return PX_FALSE;
	}
	return PX_StringFormat5(str, fmt, _1, _2, _3, _4, _5);
}

px_bool PX_StringInitializeFormat6(px_memorypool* mp, px_string* str, const px_char fmt[], px_stringformat _1, px_stringformat _2, px_stringformat _3, px_stringformat _4, px_stringformat _5, px_stringformat _6)
{
	if (!PX_StringInitialize(mp, str))
	{
		return PX_FALSE;
	}
	return PX_StringFormat6(str, fmt, _1, _2, _3, _4, _5, _6);
}

px_bool PX_StringInitializeFormat7(px_memorypool* mp, px_string* str, const px_char fmt[], px_stringformat _1, px_stringformat _2, px_stringformat _3, px_stringformat _4, px_stringformat _5, px_stringformat _6, px_stringformat _7)
{
	if (!PX_StringInitialize(mp, str))
	{
		return PX_FALSE;
	}
	return PX_StringFormat7(str, fmt, _1, _2, _3, _4, _5, _6, _7);
}

px_bool PX_StringInitializeFormat8(px_memorypool* mp, px_string* str, const px_char fmt[], px_stringformat _1, px_stringformat _2, px_stringformat _3, px_stringformat _4, px_stringformat _5, px_stringformat _6, px_stringformat _7, px_stringformat _8)
{
	if (!PX_StringInitialize(mp, str))
	{
		return PX_FALSE;
	}
	return PX_StringFormat8(str, fmt, _1, _2, _3, _4, _5, _6, _7, _8);
}

px_float PX_StringToFloat(px_string *str)
{
	return PX_atof(PX_STRING_DATA(str));
}

px_bool PX_StringIsNumeric(px_string *str)
{
	return PX_strIsNumeric(str->buffer);
}

px_bool PX_StringIsFloat(px_string *str)
{
	return PX_strIsFloat(str->buffer);
}

px_int PX_StringReadLine(const px_char content[], px_string* pout)
{
	px_int i;
	for (i = 0; content[i]; i++)
	{
		if (content[i] == '\n' || content[i] == '\r')
		{
			break;
		}
		if(!PX_StringCatChar(pout,content[i]))
		{
			return PX_FALSE;
		}
	}
	return pout->exreg_strlen;
}

px_void PX_StringCut(px_string* str, px_int left, px_int right)
{
	PX_strcut(str->buffer, left, right);
	str->exreg_strlen = PX_strlen(str->buffer);
}

px_bool PX_StringCat(px_string *str,const px_char *str2)
{
	px_uchar shl=0;
	px_char *old=str->buffer;
	px_int radius;

	radius = str->exreg_strlen + PX_strlen(str2);
	if (radius<str->bufferlen)
	{
		PX_strcat(str->buffer,str2);
		str->exreg_strlen = radius;
		return PX_TRUE;
	}

	while ((px_int)(1<<++shl)<=radius);
	str->bufferlen=(1<<shl);
	str->buffer=(px_char *)MP_Malloc(str->mp,str->bufferlen);
	if(!str->buffer)return PX_FALSE;
	str->buffer[0]='\0';
	PX_strcat(str->buffer,old);
	PX_strcat(str->buffer,str2);
	if(old)
	MP_Free(str->mp,old);
	str->exreg_strlen = radius;
	return PX_TRUE;
}

px_bool PX_StringCatInt(px_string* str, px_int value)
{
	px_char int_str[12]; // Enough to hold -2147483648 and null terminator
	PX_itoa(value, int_str, 12,10);
	return PX_StringCat(str, int_str);
}

px_bool PX_StringAppend(px_string* str, const px_char* str2)
{
	px_uchar shl = 0;
	px_char* old = str->buffer;
	px_int radius;
	px_int str2_length = PX_strlen(str2);
	radius = str->exreg_strlen + str2_length;
	if (radius < str->bufferlen)
	{
		PX_memcpy(str->buffer + str->exreg_strlen, str2, str2_length + 1); // +1 to include null terminator
		str->exreg_strlen = radius;
		return PX_TRUE;
	}

	while ((px_int)(1 << ++shl) <= radius);
	str->bufferlen = (1 << shl);
	str->buffer = (px_char*)MP_Malloc(str->mp, str->bufferlen);
	if (!str->buffer)return PX_FALSE;
	str->buffer[0] = '\0';
	PX_memcpy(str->buffer, old, str->exreg_strlen);
	PX_memcpy(str->buffer + str->exreg_strlen, str2, str2_length + 1); // +1 to include null terminator
	if (old)
		MP_Free(str->mp, old);
	str->exreg_strlen = radius;
	return PX_TRUE;
}


px_bool PX_StringCatLength(px_string* str, const px_char* str2,px_int cat_length)
{
	px_uchar shl = 0;
	px_char* old = str->buffer;
	px_int radius;

	radius = str->exreg_strlen + cat_length;
	if (radius < str->bufferlen)
	{
		PX_strcatlen(str->buffer, str2, cat_length);
		str->exreg_strlen = radius;
		return PX_TRUE;
	}

	while ((px_int)(1 << ++shl) <= radius);
	str->bufferlen = (1 << shl);
	str->buffer = (px_char*)MP_Malloc(str->mp, str->bufferlen);
	if (!str->buffer)return PX_FALSE;
	str->buffer[0] = '\0';
	PX_strcat(str->buffer, old);
	PX_strcatlen(str->buffer, str2, cat_length);
	if (old)
		MP_Free(str->mp, old);
	str->exreg_strlen = radius;
	return PX_TRUE;
}



px_char PX_StringLastChar(px_string* str)
{
	if (str->buffer&&str->buffer[0]!=0)
	{
		return str->buffer[str->exreg_strlen-1];
	}
	return 0;
}

px_void PX_StringRemoveRight(px_string* str, px_int remove_begin_index)
{
	if (remove_begin_index < 0 || remove_begin_index >= str->exreg_strlen)
	{
		PX_LOG("PX_StringRemoveRight: remove_begin_index out of range");
		return;
	}
	str->buffer[remove_begin_index] = '\0';
	str->exreg_strlen = remove_begin_index;
}

px_void PX_StringRemoveLeft(px_string* str, px_int remove_end_index)
{
	px_int tail_len;
	if (remove_end_index < 0 || remove_end_index >= str->exreg_strlen)
	{
		return;
	}
	tail_len = str->exreg_strlen - remove_end_index - 1;
	PX_memcpy(str->buffer, str->buffer + remove_end_index + 1, tail_len);
	str->buffer[tail_len] = '\0';
	str->exreg_strlen = tail_len;

}

px_void PX_StringClear(px_string *str)
{
	str->buffer[0]='\0';
	str->exreg_strlen = 0;
}

static px_bool PX_StringCatCharEx(px_string *str,px_char ch,px_int _strlen)
{
	px_uchar shl=0;
	px_char *old=str->buffer;
	px_int radius= _strlen +1;
	if (radius<str->bufferlen)
	{
		str->buffer[radius-1]=ch;
		str->buffer[radius]='\0';
		str->exreg_strlen++;
		return PX_TRUE;
	}

	while ((px_int)(1<<++shl)<=radius);
	str->bufferlen=(1<<shl);
	str->buffer=(px_char *)MP_Malloc(str->mp,str->bufferlen);
	if(!str->buffer)return PX_FALSE;
	str->buffer[0]='\0';
	PX_strcat(str->buffer,old);
	str->buffer[radius-1]=ch;
	str->buffer[radius]='\0';
	if(old)
	MP_Free(str->mp,old);

	str->exreg_strlen++;
	return PX_TRUE;
}

px_bool PX_StringCatChar(px_string* str, px_char ch)
{
	return PX_StringCatCharEx(str, ch, PX_StringLen(str));
}

px_bool PX_StringCatCharFast(px_string* str, px_char ch)
{
	return PX_StringCatCharEx(str, ch, str->exreg_strlen);
}

px_int PX_StringLen(px_string *str)
{
	return str->exreg_strlen;
}



px_bool PX_StringCopy(px_string *dest,px_string *res)
{
	if (res==dest)
	{
		PX_ERROR("px_sting Self-copied error");
		return PX_FALSE;
	}
	PX_StringClear(dest);
	return PX_StringCat(dest,res->buffer);
}

px_bool PX_StringInsertChar(px_string *str,px_int index,px_char ch)
{
	px_int cpysize=str->exreg_strlen-index;
	if (PX_StringCatChar(str,' '))
	{
		PX_memcpy(str->buffer+index+1,str->buffer+index,cpysize);
		str->buffer[index]=ch;
		return PX_TRUE;
	}
	return PX_FALSE;
}

px_bool PX_StringSetChar(px_string *pstr,px_int index,px_char ch)
{
	if (index >= 0 && index < pstr->exreg_strlen)
	{
		pstr->buffer[index] = ch;
		if (ch == '\0') pstr->exreg_strlen = index;
		return PX_TRUE;
	}
	return PX_FALSE;
}

px_bool PX_StringRemoveChar(px_string *str,px_int index)
{
	if (index>=0&&index<str->exreg_strlen)
	{
		PX_memcpy(str->buffer+index,str->buffer+index+1,str->exreg_strlen-index);
		str->exreg_strlen--;
		return PX_TRUE;
	}
	return PX_FALSE;
}

px_bool PX_StringReplace(px_string *str,const px_char *source, const px_char *replaceto)
{
	px_string tempstr;
	px_int i;
	px_int source_len=PX_strlen(source);
	if (PX_StringLen(str)==0)
	{
		return PX_TRUE;
	}
	PX_StringInitialize(str->mp,&tempstr);
	if (!PX_StringCopy(&tempstr, str))
		return PX_FALSE;
	PX_StringClear(str);

	for (i=0;i<=PX_StringLen(&tempstr)-source_len;i++)
	{
		if (PX_memequ(tempstr.buffer+i,source,source_len))
		{
			if (!PX_StringCat(str, replaceto))
			{
				PX_StringFree(&tempstr);
				return PX_FALSE;
			}
			i+=source_len-1;
		}
		else
		{
			if (!PX_StringCatChar(str, tempstr.buffer[i]))
			{
				PX_StringFree(&tempstr);
				return PX_FALSE;
			}
		}
	}
	if (!PX_StringCat(str, tempstr.buffer + i))
	{
		PX_StringFree(&tempstr);
		return PX_FALSE;
	}
	PX_StringFree(&tempstr);
	return PX_TRUE;
}

px_bool PX_StringInsert(px_string *str,px_int insertIndex,const px_char *InstrString)
{
	px_int radius=PX_strlen(InstrString);
	px_int resLen=str->exreg_strlen-insertIndex;
	if (!PX_StringCat(str, InstrString))
	{
		return PX_FALSE;
	}
	PX_memcpy(str->buffer+insertIndex+radius,str->buffer+insertIndex,resLen);
	PX_memcpy(str->buffer+insertIndex,InstrString,radius);
	return PX_TRUE;
}

px_bool PX_StringInsertToLine(px_string* str, px_int insert_to_line, const px_char* InstrString)
{
	const px_char* pstring = str->buffer;
	px_int offset = 0;
	while (insert_to_line > 0 && *pstring)
	{
		if (*pstring == '\n')
		{
			insert_to_line--;
		}
		pstring++;
		offset++;
	}
	return PX_StringInsert(str, offset, InstrString);
}


px_bool PX_StringReplaceRange(px_string *str,px_int startindex,px_int endindex,const px_char *replaceto)
{
	px_string tempStr;

	if (startindex>endindex)
	{
		PX_ERROR("string trim error");
		return PX_FALSE;
	}

	if (endindex>=str->exreg_strlen)
	{
		PX_ERROR("string trim error");
		return PX_FALSE;
	}

	if(!PX_StringInitialize(str->mp,&tempStr))
	{
		return PX_FALSE;
	}
	if(!PX_StringCopy(&tempStr,str))
	{
		PX_StringFree(&tempStr);
		return PX_FALSE;
	}

	PX_StringClear(str);

	tempStr.buffer[startindex]=0;
	PX_StringCat(str,tempStr.buffer);
	PX_StringCat(str,replaceto);
	PX_StringCat(str,tempStr.buffer+endindex+1);
	PX_StringFree(&tempStr);
	return PX_TRUE;
}

#define PX_STRING_TRIMER_REG_COUNT 16

px_bool PX_StringTrimer_Solve(px_string *pstring, const px_char *parseCode, const px_char *ReplaceCode)
{
	px_int oft,j,k,resi,repj,regindex;
	px_char regbuf[3];
	px_char endc;
	px_bool ret;
	px_string reg[PX_STRING_TRIMER_REG_COUNT],matchedString,replaceString;
	px_bool mark[PX_STRING_TRIMER_REG_COUNT],matchd;
	ret=PX_FALSE;
	for (oft=0;oft<PX_STRING_TRIMER_REG_COUNT;oft++)
	{
		PX_StringInitialize(pstring->mp,reg+oft);
		mark[oft]=PX_FALSE;
	}

	PX_StringInitialize(pstring->mp,&matchedString);

	oft=0;
	
	while(pstring->buffer[oft])
	{
		repj=0;
		resi=oft;
		matchd=PX_FALSE;

		while(PX_TRUE)
		{
			
			if (parseCode[repj]=='%')
			{
				j=0;
				repj++;

				PX_StringClear(&matchedString);

				while(PX_TRUE)
				{
					if (parseCode[repj]>='0'&&parseCode[repj]<='9')
					{
						regbuf[j++]=parseCode[repj++];
						if (j>2)
						{
							goto _DONE;
						}
						regbuf[j]=0;
						
					}
					else
						break;
					
				}
				if (j==0)
				{
					goto _DONE;
				}
				regindex=PX_atoi(regbuf);
				if (regindex>=PX_STRING_TRIMER_REG_COUNT)
				{
					goto _DONE;
				}
				endc=parseCode[repj];

				while (PX_TRUE)
				{
					if (pstring->buffer[resi]==endc)
					{
						break;
					}
					if (pstring->buffer[resi]=='\0')
					{
						goto _DONE;
					}
					PX_StringCatChar(&matchedString,pstring->buffer[resi]);
					resi++;
				}

				if (mark[regindex])
				{
					//reg 0 as wildcard
					if (PX_strequ(reg[regindex].buffer,matchedString.buffer)||!regindex)
					{
						matchd=PX_TRUE;
					}
					else
					{
						matchd=PX_FALSE;
						break;
					}
				}
				else
				{
					PX_StringCopy(&reg[regindex],&matchedString);
					mark[regindex]=PX_TRUE;
				}
				
			}
			else
			{
				if (parseCode[repj]=='\0')
				{
					matchd=PX_TRUE;
					break;
				}
				if (pstring->buffer[resi]=='\0')
				{
					break;
				}
				if (pstring->buffer[resi]==parseCode[repj])
				{
					resi++;
					repj++;
				}
				else
					break;
			}
		}



		if (matchd)
		{
			PX_StringInitialize(pstring->mp,&replaceString);
			j=0;
			k=0;
			while(PX_TRUE)
			{
				if (ReplaceCode[j]==0)
				{
					break;
				}
				if (ReplaceCode[j]=='%')
				{
					j++;
					k=0;
					while(PX_TRUE)
					{
						if (ReplaceCode[j]>='0'&&ReplaceCode[j]<='9')
						{
							regbuf[k++]=ReplaceCode[j++];
							
							if (k>2)
							{
								PX_StringFree(&replaceString);
								goto _DONE;
							}

							regbuf[k]='\0';
							
							regindex=PX_atoi(regbuf);
							if (regindex>=PX_STRING_TRIMER_REG_COUNT)
							{
								PX_StringFree(&replaceString);
								goto _DONE;
							}
							PX_StringCat(&replaceString,reg[regindex].buffer);
						}
						else
							break;
					}
				}
				else
				{
					PX_StringCatChar(&replaceString,ReplaceCode[j++]);
				}
			}


			PX_StringReplaceRange(pstring,oft,resi-1,replaceString.buffer);
			ret=PX_TRUE;

			oft+=replaceString.exreg_strlen;

			PX_StringFree(&replaceString);
		}
		else
		{
			oft++;
		}

		for (k=0;k<PX_STRING_TRIMER_REG_COUNT;k++)
		{
			mark[k]=PX_FALSE;
		}
	}

_DONE:
	for (oft=0;oft<PX_STRING_TRIMER_REG_COUNT;oft++)
	{
		PX_StringFree(reg+oft);
	}
	PX_StringFree(&matchedString);
	return ret;
}


px_void PX_StringFixUncompleteCode(px_string* text)
{
	PX_FontTrimUncompletedUTF8String(text->buffer);
	text->exreg_strlen = PX_strlen(text->buffer);
}

px_void PX_StringUpdateExReg(px_string *text)
{
	if (text->buffer)
	{
		text->exreg_strlen =PX_strlen(text->buffer);
	}
	else
		text->exreg_strlen =0;
}



px_void PX_StringBackspace(px_string* text)
{
	px_int len = PX_strlen(text->buffer);
	if (len)
	{
		text->buffer[len - 1] = '\0';
		text->exreg_strlen = len - 1;
	}
}


px_void PX_StringBackspaceEx(px_string* text)
{
	if (text->exreg_strlen > 0)
	{
		text->buffer[text->exreg_strlen - 1] = '\0';
		text->exreg_strlen--;
	}
}



px_bool PX_StringCatFormat(px_string* text, const px_char fmt[], px_stringformat _1, px_stringformat _2, px_stringformat _3, px_stringformat _4, px_stringformat _5, px_stringformat _6, px_stringformat _7, px_stringformat _8)
{
	px_char* oldptr;
	px_int finalLen = 0;
	px_char shl = 0;
	finalLen = PX_sprintf8(PX_NULL, 0, fmt, _1, _2, _3, _4, _5, _6, _7, _8)+text->exreg_strlen;
	if (finalLen>=text->bufferlen)
	{
		oldptr = text->buffer;
		while ((1 << ++shl) <= finalLen);
		text->bufferlen = (1 << shl);
		text->buffer = (px_char*)MP_Malloc(text->mp, text->bufferlen);
		if (!text->buffer)
		{
			text->exreg_strlen = 0;
			return PX_FALSE;
		}
		PX_strset(text->buffer, oldptr);
		if (oldptr)
			MP_Free(text->mp, oldptr);
	}
	PX_sprintf8(text->buffer+text->exreg_strlen, text->bufferlen-text->exreg_strlen, fmt, _1, _2, _3, _4, _5, _6, _7, _8);
	text->exreg_strlen = finalLen;
	return PX_TRUE;

}
px_bool PX_StringCatFormat7(px_string* text, const px_char fmt[], px_stringformat _1, px_stringformat _2, px_stringformat _3, px_stringformat _4, px_stringformat _5, px_stringformat _6, px_stringformat _7)
{
		return PX_StringCatFormat(text, fmt, _1, _2, _3, _4, _5, _6, _7, PX_STRINGFORMAT_INT(0));

}
px_bool PX_StringCatFormat6(px_string* text, const px_char fmt[], px_stringformat _1, px_stringformat _2, px_stringformat _3, px_stringformat _4, px_stringformat _5, px_stringformat _6)
{
	return PX_StringCatFormat(text, fmt, _1, _2, _3, _4, _5, _6, PX_STRINGFORMAT_INT(0), PX_STRINGFORMAT_INT(0));
}
px_bool PX_StringCatFormat5(px_string* text, const px_char fmt[], px_stringformat _1, px_stringformat _2, px_stringformat _3, px_stringformat _4, px_stringformat _5)
{
	return PX_StringCatFormat(text, fmt, _1, _2, _3, _4, _5, PX_STRINGFORMAT_INT(0), PX_STRINGFORMAT_INT(0), PX_STRINGFORMAT_INT(0));
}
px_bool PX_StringCatFormat4(px_string* text, const px_char fmt[], px_stringformat _1, px_stringformat _2, px_stringformat _3, px_stringformat _4)
{
	return PX_StringCatFormat(text, fmt, _1, _2, _3, _4, PX_STRINGFORMAT_INT(0), PX_STRINGFORMAT_INT(0), PX_STRINGFORMAT_INT(0), PX_STRINGFORMAT_INT(0));
}
px_bool PX_StringCatFormat3(px_string* text, const px_char fmt[], px_stringformat _1, px_stringformat _2, px_stringformat _3)
{
	return PX_StringCatFormat(text, fmt, _1, _2, _3, PX_STRINGFORMAT_INT(0), PX_STRINGFORMAT_INT(0), PX_STRINGFORMAT_INT(0), PX_STRINGFORMAT_INT(0), PX_STRINGFORMAT_INT(0));
}
px_bool PX_StringCatFormat2(px_string* text, const px_char fmt[], px_stringformat _1, px_stringformat _2)
{
	return PX_StringCatFormat(text, fmt, _1, _2, PX_STRINGFORMAT_INT(0), PX_STRINGFORMAT_INT(0), PX_STRINGFORMAT_INT(0), PX_STRINGFORMAT_INT(0), PX_STRINGFORMAT_INT(0), PX_STRINGFORMAT_INT(0));
}
px_bool PX_StringCatFormat1(px_string* text, const px_char fmt[], px_stringformat _1)
{
	return PX_StringCatFormat(text, fmt, _1, PX_STRINGFORMAT_INT(0), PX_STRINGFORMAT_INT(0), PX_STRINGFORMAT_INT(0), PX_STRINGFORMAT_INT(0), PX_STRINGFORMAT_INT(0), PX_STRINGFORMAT_INT(0), PX_STRINGFORMAT_INT(0));
}

px_bool PX_StringNumeric_add(const px_char operand1[], const px_char operand2[], px_char out[],px_int outsize)
{
	px_int i,index;
	px_char n1[32] = { 0 }, n2[32] = { 0 };
	px_char result[33] = {0};
	px_int e1=0, e2=0,e;
	px_int n1_is_negative = 0, n2_is_negative = 0;
	px_int longest,long1=0,long2 =0;

	PX_ASSERTIFX(operand1[0] == 0 || operand2[0] == 0, "PX_StringNumeric_add: empty string");
	PX_ASSERTIFX(PX_strlen(operand1) >= 32, "PX_StringNumeric_add: string too long");
	PX_ASSERTIFX(PX_strlen(operand2) >= 32, "PX_StringNumeric_add: string too long");


	if (operand1[0]=='-')
	{
		n1_is_negative = 1;
	}
	if (operand2[0] == '-')
	{
		n2_is_negative = 1;
	}
	index = 0;
	for (i = PX_strlen(operand1)-1; i >= n1_is_negative; i--)
	{
		if (operand1[i] != '.')
		{
			if (e1 >= 0)
				e1++;
			n1[index++] = operand1[i] - '0';
			long1++;
		}
		else
		{
			e1 = -e1;
		}
	}
	if (e1>0)
	{
		e1 = 0;
	}
	index = 0;
	for (i = PX_strlen(operand2)-1; i >= n2_is_negative; i--)
	{
		if (operand2[i] != '.')
		{
			if (e2 >= 0)
				e2++;
			n2[index++] = operand2[i] - '0';
			long2++;
		}
		else
		{
			e2 = -e2;
		}
	}
	if (e2 > 0)
	{
		e2 = 0;
	}
	e = e2;
	if (e1 < e2)
	{
		PX_memcpy(n2 + e2 - e1, n2, PX_strlen(n2));
		PX_memset(n2, 0, e2 - e1);
		e = e1;
	}
	else if (e1 > e2)
	{
		PX_memcpy(n1 + e1 - e2, n1, PX_strlen(n1));
		PX_memset(n1, 0, e1 - e2);
	}
	longest = long1 > long2 ? long1 : long2;

	if (n1_is_negative==n2_is_negative)
	{
		px_int carry = 0;
		for (i = 0; i < longest; i++)
		{
			px_int t = n1[i] + n2[i] + carry;
			if (t > 0)
			{
				result[i] = (t % 10) + '0';
				carry = t / 10;
			}
			else if (t < 0)
			{
				t += 10;
				result[i] = (t % 10) + '0';
				carry = -1;
			}
			else
			{
				result[i] = '0';
				carry = 0;
			}
		}
		
		if (carry > 0)
		{
			result[i] = carry + '0';
			i++;
		}

		while (i>1 && result[i - 1] == '0')
		{
			result[i - 1] = '\0';
			i--;
		}

		if (n1_is_negative)
		{
			result[PX_strlen(result)] = '-';
		}
	}
	else
	{
		px_int carry = 0;
		px_int negative=PX_FALSE;
		if (n1_is_negative)
		{
			for (i = 0; i < longest; i++)
			{
				n1[i] = - n1[i];
			}
		}
		else
		{
			for (i = 0; i < longest; i++)
			{
				n2[i] = -n2[i];
			}
		}
		
		for (i = 0; i < longest; i++)
		{
			px_int t = n1[i] + n2[i] + carry;
			if (t < 0)
			{
				t += 10;
				carry = -1;
			}
			else
			{
				carry = 0;
			}
			result[i] = (t % 10) + '0';
		}
		

		if (carry < 0)
		{
			negative = PX_TRUE;
			for (i = 0; i < longest; i++)
			{
				n1[i] = -n1[i];
				n2[i] = -n2[i];
			}
			PX_memset(result, 0, sizeof(result));
			carry = 0;
			for (i = 0; i < longest; i++)
			{
				px_int t = n1[i] + n2[i] + carry;
				if (t > 0)
				{
					result[i] = (t % 10) + '0';
					carry = t / 10;
				}
				else if (t < 0)
				{
					t += 10;
					result[i] = (t % 10) + '0';
					carry = -1;
				}
				else
				{
					result[i] = '0';
					carry = 0;
				}
			}
		}
		while(i>1 && result[i - 1] == '0')
		{
			result[i - 1] = '\0';
			i--;
		}
		if (result[0]=='0'&& result[1]=='\0')
		{
			out[0] = '0';
			out[1] = '\0';
			return PX_TRUE;
		}

		if (negative)
		{
			result[PX_strlen(result)] = '-';
		}
	}
	if (PX_strlen(result) >= outsize)
	{
		return PX_FALSE;
	}
	index = 0;

	for (i = PX_strlen(result)-1; i >=0; i--)
	{
		if (i == -e - 1)
		{
			out[index++] = '.';
		}
		out[index++] = result[i];
	}
	out[index] = '\0';

	return PX_TRUE;
}

px_bool PX_StringNumeric_sub(const px_char operand1[], const px_char operand2[], px_char result[], px_int outsize)
{
	px_char n2[32] = { 0 };
	PX_ASSERTIFX(operand1[0] == 0 || operand2[0] == 0, "PX_StringNumeric_sub: empty string");
	PX_ASSERTIFX(PX_strlen(operand1) >= 32, "PX_StringNumeric_sub: string too long");
	PX_ASSERTIFX(PX_strlen(operand2) >= 32, "PX_StringNumeric_sub: string too long");
	//sub as add with negative number
	if (operand2[0] == '-')
	{
		PX_strset(n2, operand2 + 1);
	}
	else
	{
		n2[0] = '-';
		PX_strset(n2 + 1, operand2);
	}
	return PX_StringNumeric_add(operand1, n2, result, outsize);
}

px_bool PX_StringNumeric_mul(const px_char operand1[], const px_char operand2[], px_char out[], px_int outsize)
{
	px_int i, index,offset;
	px_char n1[32] = { 0 }, n2[32] = { 0 };
	px_int result[64] = { 0 };
	px_char cresult[64] = { 0 };
	px_int e1 = 0, e2 = 0, e;
	px_int n1_is_negative = 0, n2_is_negative = 0;
	px_int long1 = 0, long2 = 0;

	PX_ASSERTIFX(operand1[0] == 0 || operand2[0] == 0, "PX_StringNumeric_add: empty string");
	PX_ASSERTIFX(PX_strlen(operand1) >= 32, "PX_StringNumeric_add: string too long");
	PX_ASSERTIFX(PX_strlen(operand2) >= 32, "PX_StringNumeric_add: string too long");


	if (operand1[0] == '-')
	{
		n1_is_negative = 1;
	}
	if (operand2[0] == '-')
	{
		n2_is_negative = 1;
	}
	index = 0;
	for (i = PX_strlen(operand1) - 1; i >= n1_is_negative; i--)
	{
		if (operand1[i] != '.')
		{
			if (e1 >= 0)
				e1++;
			n1[index++] = operand1[i] - '0';
			long1++;
		}
		else
		{
			e1 = -e1;
		}
	}
	if (e1 > 0)
	{
		e1 = 0;
	}
	index = 0;
	offset = 0;
	for (i = PX_strlen(operand2) - 1; i >= n2_is_negative; i--)
	{
		if (operand2[i] != '.')
		{
			if (e2 >= 0)
				e2++;
			n2[index++] = operand2[i] - '0';
			long2++;
		}
		else
		{
			e2 = -e2;
		}
	}
	if (e2 > 0)
	{
		e2 = 0;
	}
	e = e1 + e2;
	if (long1 + long2 + 1 >= outsize)
	{
		return PX_FALSE;
	}
	for ( i = 0; i < long2; i++)
	{
		px_int temp_result[33] = { 0 };
		px_int carry = 0;
		px_int j,k;
		for (j = 0; j < long1; j++)
		{
			px_int t = n2[i] * n1[j] + carry;
			if (t > 0)
			{
				temp_result[j] = (t % 10);
				carry = t / 10;
			}
			else
			{
				temp_result[j] = 0;
				carry = 0;
			}
		}
		if (carry > 0)
		{
			temp_result[j] = carry;
			j++;
		}
		for (k = 0; k < j; k++)
		{
			result[k + offset] += temp_result[k];
		}
		offset++;
	}
	for (i = 0; i < PX_COUNTOF(result) - 1; i++)
	{
		if (result[i] >= 10)
		{
			cresult[i] = (result[i] % 10) + '0';
			result[i + 1] += result[i] / 10;
			result[i] = result[i] % 10;
		}
		else
		{
			cresult[i] = result[i] + '0';
		}
	}
	if (result[i]>=10)
	{
		return PX_FALSE;
	}
	else
	{
		cresult[i] = result[i];
	}
	while (i > 1 && i>-e && cresult[i - 1] == '0')
	{
		cresult[i - 1] = '\0';
		i--;
	}
	if (cresult[0] == '0' && cresult[1] == '\0')
	{
		out[0] = '0';
		out[1] = '\0';
		return PX_TRUE;
	}
	if (n1_is_negative != n2_is_negative)
	{
		cresult[i] = '-';
	}
	
	index = 0;
	for (i = PX_strlen(cresult)-1; i >= 0; i--)
	{
		if (i == -e - 1)
		{
			if (index==0)
			{
				out[index++] = '0';
			}
			out[index++] = '.';
		}
		out[index++] = cresult[i] ;
	}



	out[index] = '\0';
	return PX_TRUE;
}
px_bool PX_StringNumeric_div2(const px_char operand1[], const px_char operand2[], px_char out[], px_int outsize)
{
	px_double d1, d2;
	d1 = PX_atof64(operand1);
	d2 = PX_atof64(operand2);
	if (d2 == 0)
	{
		return PX_FALSE;
	}
	return PX_ftoa64(d1 / d2, out, outsize, 16);
}

px_bool PX_StringNumeric_div(const px_char operand1[], const px_char operand2[], px_char out[], px_int outsize)
{

	px_int i, index, offset,woffset;
	px_char n1[32] = { 0 }, n2[12] = { 0 };
	px_int e1 = 0, e2 = 0, e;
	px_int n1_is_negative = 0, n2_is_negative = 0;
	px_int op1_len, op2_len;
	px_uint uop1=0,uop2=0;
	px_bool first_nonzero_flag = PX_FALSE;
	px_int dotpos = outsize+1;
	PX_memset(out, 0, outsize);
	op1_len = PX_strlen(operand1);
	op2_len = PX_strlen(operand2);
	if(operand1[0] == 0 || operand2[0] == 0)return PX_FALSE;
	if (op1_len >= 32)return PX_FALSE;
	if(op2_len > 10)return PX_FALSE;

	if (operand1[0] == '-')
		n1_is_negative = 1;

	if (operand2[0] == '-')
		n2_is_negative = 1;
	
	e1 = 0;
	for (i = n1_is_negative; i < op1_len; i++)
	{
		if (operand1[i] == '.')
		{
			e1 = -op1_len + i + 1;
			break;
		}
	}

	index = 0;
	for (i= n1_is_negative;i<op1_len;i++)
		if (operand1[i] != '.')
		{
			n1[index] = operand1[i];
			if (index == 0 && operand1[i] == '0')
			{
				continue;
			}
			index++;
		}
	
	
	for (i = op2_len - 1; i >= n2_is_negative; i--)
	{
		if (operand2[i] == '.')
			break;
		e2++;
	}
	if (i< n2_is_negative)
	{
		e2 = 0;
	}

	index = 0;
	for (i = n2_is_negative; i < op2_len; i++)
	{
		if (operand2[i] != '.')
		{
			n2[index] = operand2[i];
			if (index == 0 && operand2[i] == '0')
			{
				continue;
			}
			index++;
		}
	}
	e = e1 + e2 + PX_strlen(n1);
	uop2 = PX_atoi(n2);
	if (uop2==0)
	{
		return PX_FALSE;
	}

	offset = 0;
	woffset = 0;
	if (e<0)
	{
		if (outsize<=3)
		{
			return PX_FALSE;
		}
		out[woffset++] = '0';
		out[woffset++] = '.';
		dotpos = 1;
	}
	while (PX_TRUE)
	{
		if (e==0)
		{
			if (woffset==0)
			{
				if (outsize - woffset <= 2)
				{
					return PX_FALSE;
				}
				out[woffset++] = '0';
			}
			dotpos = woffset;
			out[woffset++] = '.';
			if (woffset >= outsize - 1)
				return PX_FALSE;
		}
		if(n1[offset]!=0)
		{
			uop1 = uop1*10+ n1[offset]-'0';
			offset++;
		}
		else
		{
			uop1 *= 10;
		}
		e--;
		if (uop1==0&&e>=0&& first_nonzero_flag)
		{
			out[woffset++] = '0';
		}
		else if (uop1>=uop2)
		{
			first_nonzero_flag = PX_TRUE;
			out[woffset++] = (uop1 / uop2) + '0';
			uop1 = uop1 % uop2;
		}
		else
		{
			if (first_nonzero_flag)
				out[woffset++] = '0';
			else if (e < 0)
				out[woffset++] = '0';
		}
		if (woffset >= outsize - 2)
		{
			if (e < 0)
				break;
			else
				return PX_FALSE;
		}
		if (uop1 == 0 && n1[offset] == 0&&e<=0)
		{
			break;
		}
	}
	if (woffset==0)
	{
		if (outsize < 2) return PX_FALSE;
		out[0] = '0';
		out[1] = 0;
		return PX_TRUE;
	}
	woffset--;
	while (woffset> dotpos)
	{
		if (out[woffset]=='0')
		{
			if (out[woffset-1]=='.')
			{
				return PX_TRUE;
			}
			out[woffset--] = '\0';
		}
		else
		{
			break;
		}
	}
	if (n1_is_negative!=n2_is_negative)
	{
		if (woffset+2 >= outsize)
			return PX_FALSE;
		for (i = woffset; i >= 0; i--)
		{
			out[i + 1] = out[i];
		}
		out[0] = '-';
		out[woffset + 2] = '\0';
	}
	return PX_TRUE;
}
/*
px_bool PX_StringNumeric_and(const px_char operand1[32], const px_char operand2[32], const px_char result[32]);
px_bool PX_StringNumeric_or(const px_char operand1[32], const px_char operand2[32], const px_char result[32]);
px_bool PX_StringNumeric_xor(const px_char operand1[32], const px_char operand2[32], const px_char result[32]);
px_bool PX_StringNumeric_shl(const px_char operand1[32], const px_char operand2[32], const px_char result[32]);
px_bool PX_StringNumeric_shr(const px_char operand1[32], const px_char operand2[32], const px_char result[32]);
px_bool PX_StringNumeric_neg(const px_char operand1[32], const px_char result[32]);
*/
