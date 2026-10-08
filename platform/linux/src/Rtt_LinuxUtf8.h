//////////////////////////////////////////////////////////////////////////////
//
// This file is part of the Corona game engine.
// For overview and more information on licensing please refer to README.md
// Home page: https://coronalabs.com
//
// UTF-8 validation and boundary-safe splitting. Header-only and free of SDL
// so it can be tested on its own.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef _Rtt_LinuxUtf8_H__
#define _Rtt_LinuxUtf8_H__

#include <stddef.h>
#include <string>
#include <vector>

namespace Rtt
{
	// True if s is well-formed UTF-8: no stray continuation bytes, no
	// truncated sequences, no overlong forms, no surrogates (U+D800..DFFF)
	// and nothing above U+10FFFF. On success *codepoints holds the number of
	// characters; on failure *badOffset is the byte where the first invalid
	// sequence starts. Either pointer may be NULL.
	inline bool Utf8Validate(const std::string& s, size_t* badOffset, size_t* codepoints)
	{
		const unsigned char* p = (const unsigned char*)s.data();
		const size_t n = s.size();
		size_t i = 0;
		size_t count = 0;
		while (i < n)
		{
			const unsigned char c = p[i];
			size_t len;
			unsigned char lo = 0x80, hi = 0xBF;  // allowed range of the 2nd byte
			if (c < 0x80)
			{
				len = 1;
			}
			else if (c >= 0xC2 && c <= 0xDF)
			{
				len = 2;
			}
			else if (c >= 0xE0 && c <= 0xEF)
			{
				len = 3;
				if (c == 0xE0) { lo = 0xA0; }       // overlong
				else if (c == 0xED) { hi = 0x9F; }  // surrogates
			}
			else if (c >= 0xF0 && c <= 0xF4)
			{
				len = 4;
				if (c == 0xF0) { lo = 0x90; }       // overlong
				else if (c == 0xF4) { hi = 0x8F; }  // above U+10FFFF
			}
			else
			{
				// 0x80..0xC1 (continuation without a lead, or overlong
				// two-byte lead) and 0xF5..0xFF.
				if (badOffset) { *badOffset = i; }
				return false;
			}

			if (i + len > n)
			{
				if (badOffset) { *badOffset = i; }
				return false;
			}
			for (size_t k = 1; k < len; ++k)
			{
				const unsigned char cc = p[i + k];
				const unsigned char min = (k == 1) ? lo : 0x80;
				const unsigned char max = (k == 1) ? hi : 0xBF;
				if (cc < min || cc > max)
				{
					if (badOffset) { *badOffset = i; }
					return false;
				}
			}
			i += len;
			++count;
		}
		if (codepoints) { *codepoints = count; }
		return true;
	}

	// Splits valid UTF-8 into pieces of at most maxBytes bytes each, cutting
	// only between characters, so every piece is itself valid UTF-8 and the
	// pieces concatenate back to s. Expects valid input (see Utf8Validate)
	// and maxBytes >= 4.
	inline std::vector<std::string> Utf8SplitChunks(const std::string& s, size_t maxBytes)
	{
		std::vector<std::string> chunks;
		size_t start = 0;
		while (start < s.size())
		{
			size_t end = start + maxBytes;
			if (end >= s.size())
			{
				end = s.size();
			}
			else
			{
				// Back up over continuation bytes to the start of the
				// character that would be cut.
				while (end > start && ((unsigned char)s[end] & 0xC0) == 0x80)
				{
					--end;
				}
			}
			chunks.push_back(s.substr(start, end - start));
			start = end;
		}
		return chunks;
	}
}

#endif // _Rtt_LinuxUtf8_H__
