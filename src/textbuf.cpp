/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file textbuf.cpp Textbuffer handling. */

#include "stdafx.h"
#include <stdarg.h>

#include "textbuf_type.h"
#include "string.h"
#include "strings_func.h"
#include "gfx_type.h"
#include "gfx_func.h"
#include "window_func.h"
#include "core/alloc_func.hpp"
#include "core/smallvec_type.hpp"
#include "language.h"

/**
 * Try to retrieve the current clipboard contents.
 *
 * @note OS-specific function.
 * @return True if some text could be retrieved.
 */
bool GetClipboardContents(char *buffer, size_t buff_len);

int _caret_timer;


/** Base class for iterating over different kind of parts of a string. */
struct BaseStringIterator {
	/** Type of the iterator. */
	enum IterType {
		ITER_CHARACTER, ///< Iterate over characters (or more exactly grapheme clusters).
		ITER_WORD,      ///< Iterate over words.
	};

	/** Sentinel to indicate end-of-iteration. */
	static const size_t END = SIZE_MAX;
};


#ifdef WITH_ICU

#include <unicode/utext.h>
#include <unicode/brkiter.h>

/** String iterator using ICU as a backend. */
class IcuStringIterator : public BaseStringIterator
{
	icu::BreakIterator *char_itr; ///< ICU iterator for characters.
	icu::BreakIterator *word_itr; ///< ICU iterator for words.

	SmallVector<UChar, 32> utf16_str;      ///< UTF-16 copy of the string.
	SmallVector<size_t, 32> utf16_to_utf8; ///< Mapping from UTF-16 code point position to index in the UTF-8 source string.

public:
	IcuStringIterator() : char_itr(NULL), word_itr(NULL)
	{
		UErrorCode status = U_ZERO_ERROR;
		this->char_itr = icu::BreakIterator::createCharacterInstance(icu::Locale(_current_language != NULL ? _current_language->isocode : "en"), status);
		this->word_itr = icu::BreakIterator::createWordInstance(icu::Locale(_current_language != NULL ? _current_language->isocode : "en"), status);

		*this->utf16_str.Append() = '\0';
		*this->utf16_to_utf8.Append() = 0;
	}

	~IcuStringIterator()
	{
		delete this->char_itr;
		delete this->word_itr;
	}

	void SetString(const char *s)
	{
		const char *string_base = s;

		/* Unfortunately current ICU versions only provide rudimentary support
		 * for word break iterators (especially for CJK languages) in combination
		 * with UTF-8 input. As a work around we have to convert the input to
		 * UTF-16 and create a mapping back to UTF-8 character indices. */
		this->utf16_str.Clear();
		this->utf16_to_utf8.Clear();

		while (*s != '\0') {
			size_t idx = s - string_base;

			WChar c = Utf8Consume(&s);
			if (c <	0x10000) {
				*this->utf16_str.Append() = (UChar)c;
			} else {
				/* Make a surrogate pair. */
				*this->utf16_str.Append() = (UChar)(0xD800 + ((c - 0x10000) >> 10));
				*this->utf16_str.Append() = (UChar)(0xDC00 + ((c - 0x10000) & 0x3FF));
				*this->utf16_to_utf8.Append() = idx;
			}
			*this->utf16_to_utf8.Append() = idx;
		}
		*this->utf16_str.Append() = '\0';
		*this->utf16_to_utf8.Append() = s - string_base;

		UText text = UTEXT_INITIALIZER;
		UErrorCode status = U_ZERO_ERROR;
		utext_openUChars(&text, this->utf16_str.Begin(), this->utf16_str.Length() - 1, &status);
		this->char_itr->setText(&text, status);
		this->word_itr->setText(&text, status);
		this->char_itr->first();
		this->word_itr->first();
	}

	size_t SetCurPosition(size_t pos)
	{
		/* Convert incoming position to an UTF-16 string index. */
		uint utf16_pos = 0;
		for (uint i = 0; i < this->utf16_to_utf8.Length(); i++) {
			if (this->utf16_to_utf8[i] == pos) {
				utf16_pos = i;
				break;
			}
		}

		/* isBoundary has the documented side-effect of setting the current
		 * position to the first valid boundary equal to or greater than
		 * the passed value. */
		this->char_itr->isBoundary(utf16_pos);
		return this->utf16_to_utf8[this->char_itr->current()];
	}

	size_t Next(IterType what)
	{
		int32_t pos;
		switch (what) {
			case ITER_CHARACTER:
				pos = this->char_itr->next();
				break;

			case ITER_WORD:
				pos = this->word_itr->following(this->char_itr->current());
				/* The ICU word iterator considers both the start and the end of a word a valid
				 * break point, but we only want word starts. Move to the next location in
				 * case the new position points to whitespace. */
				while (pos != icu::BreakIterator::DONE &&
						IsWhitespace(Utf16DecodeChar((const uint16 *)&this->utf16_str[pos]))) {
					int32_t new_pos = this->word_itr->next();
					/* Don't set it to DONE if it was valid before. Otherwise we'll return END
					 * even though the iterator wasn't at the end of the string before. */
					if (new_pos == icu::BreakIterator::DONE) break;
					pos = new_pos;
				}

				this->char_itr->isBoundary(pos);
				break;

			default:
				NOT_REACHED();
		}

		return pos == icu::BreakIterator::DONE ? END : this->utf16_to_utf8[pos];
	}

	size_t Prev(IterType what)
	{
		int32_t pos;
		switch (what) {
			case ITER_CHARACTER:
				pos = this->char_itr->previous();
				break;

			case ITER_WORD:
				pos = this->word_itr->preceding(this->char_itr->current());
				/* The ICU word iterator considers both the start and the end of a word a valid
				 * break point, but we only want word starts. Move to the previous location in
				 * case the new position points to whitespace. */
				while (pos != icu::BreakIterator::DONE &&
						IsWhitespace(Utf16DecodeChar((const uint16 *)&this->utf16_str[pos]))) {
					int32_t new_pos = this->word_itr->previous();
					/* Don't set it to DONE if it was valid before. Otherwise we'll return END
					 * even though the iterator wasn't at the start of the string before. */
					if (new_pos == icu::BreakIterator::DONE) break;
					pos = new_pos;
				}

				this->char_itr->isBoundary(pos);
				break;

			default:
				NOT_REACHED();
		}

		return pos == icu::BreakIterator::DONE ? END : this->utf16_to_utf8[pos];
	}
};

#else

/** Fallback simple string iterator. */
class DefaultStringIterator : public BaseStringIterator
{
	const char *string; ///< Current string.
	size_t len;         ///< String length.
	size_t cur_pos;     ///< Current iteration position.

public:
	DefaultStringIterator() : string(NULL), len(0), cur_pos(0)
	{
	}

	void SetString(const char *s)
	{
		this->string = s;
		this->len = strlen(s);
		this->cur_pos = 0;
	}

	size_t SetCurPosition(size_t pos)
	{
		assert(this->string != NULL && pos <= this->len);
		/* Sanitize in case we get a position inside an UTF-8 sequence. */
		while (pos > 0 && IsUtf8Part(this->string[pos])) pos--;
		return this->cur_pos = pos;
	}

	size_t Next(IterType what)
	{
		assert(this->string != NULL);

		/* Already at the end? */
		if (this->cur_pos >= this->len) return END;

		switch (what) {
			case ITER_CHARACTER: {
				WChar c;
				this->cur_pos += Utf8Decode(&c, this->string + this->cur_pos);
				return this->cur_pos;
			}

			case ITER_WORD: {
				WChar c;
				/* Consume current word. */
				size_t offs = Utf8Decode(&c, this->string + this->cur_pos);
				while (this->cur_pos < this->len && !IsWhitespace(c)) {
					this->cur_pos += offs;
					offs = Utf8Decode(&c, this->string + this->cur_pos);
				}
				/* Consume whitespace to the next word. */
				while (this->cur_pos < this->len && IsWhitespace(c)) {
					this->cur_pos += offs;
					offs = Utf8Decode(&c, this->string + this->cur_pos);
				}

				return this->cur_pos;
			}

			default:
				NOT_REACHED();
		}

		return END;
	}

	size_t Prev(IterType what)
	{
		assert(this->string != NULL);

		/* Already at the beginning? */
		if (this->cur_pos == 0) return END;

		switch (what) {
			case ITER_CHARACTER:
				return this->cur_pos = Utf8PrevChar(this->string + this->cur_pos) - this->string;

			case ITER_WORD: {
				const char *s = this->string + this->cur_pos;
				WChar c;
				/* Consume preceding whitespace. */
				do {
					s = Utf8PrevChar(s);
					Utf8Decode(&c, s);
				} while (s > this->string && IsWhitespace(c));
				/* Consume preceding word. */
				while (s > this->string && !IsWhitespace(c)) {
					s = Utf8PrevChar(s);
					Utf8Decode(&c, s);
				}
				/* Move caret back to the beginning of the word. */
				if (IsWhitespace(c)) Utf8Consume(&s);

				return this->cur_pos = s - this->string;
			}

			default:
				NOT_REACHED();
		}

		return END;
	}
};

#endif


/**
 * Checks if it is possible to delete a character.
 * @param backspace if set, delete the character before the caret,
 * otherwise, delete the character after it.
 * @return true if a character can be deleted in the given direction.
 */
bool Textbuf::CanDelChar(bool backspace)
{
	return backspace ? this->caretpos != 0 : this->caretpos < this->length();
}

/**
 * Delete a character from a textbuffer, either with 'Delete' or 'Backspace'
 * The character is delete from the position the caret is at
 * @param keycode Type of deletion, either WKC_BACKSPACE or WKC_DELETE
 * @return Return true on successful change of Textbuf, or false otherwise
 */
bool Textbuf::DeleteChar(uint16 keycode)
{
	bool word = (keycode & WKC_CTRL) != 0;

	keycode &= ~WKC_SPECIAL_KEYS;
	if (keycode != WKC_BACKSPACE && keycode != WKC_DELETE) return false;

	bool backspace = keycode == WKC_BACKSPACE;

	if (!CanDelChar(backspace)) return false;

	char *s = this->buffer + this->caretpos;
	uint16 len = 0;

	if (word) {
		/* Delete a complete word. */
		if (backspace) {
			/* Delete whitespace and word in front of the caret. */
			len = this->caretpos - (uint16)this->char_iter->Prev(StringIterator::ITER_WORD);
			s -= len;
		} else {
			/* Delete word and following whitespace following the caret. */
			len = (uint16)this->char_iter->Next(StringIterator::ITER_WORD) - this->caretpos;
		}
		/* Update character count. */
		for (const char *ss = s; ss < s + len; Utf8Consume(&ss)) {
			this->chars--;
		}
	} else {
		/* Delete a single character. */
		if (backspace) {
			/* Delete the last code point in front of the caret. */
			s = Utf8PrevChar(s);
			WChar c;
			len = (uint16)Utf8Decode(&c, s);
			this->chars--;
		} else {
			/* Delete the complete character following the caret. */
			len = (uint16)this->char_iter->Next(StringIterator::ITER_CHARACTER) - this->caretpos;
			/* Update character count. */
			for (const char *ss = s; ss < s + len; Utf8Consume(&ss)) {
				this->chars--;
			}
		}
	}

	/* Move the remaining characters over the marker */
	memmove(s, s + len, this->len + 1 - (s - this->buffer) - len);
	this->len -= len;

	if (backspace) this->caretpos -= len;

	this->UpdateStringIter();
	this->UpdateWidth();
	this->UpdateCaretPosition();
	this->UpdateMarkedText();

	return true;
}

/**
 * Delete every character in the textbuffer
 */
void Textbuf::DeleteAll()
{
	this->zerofill();
	this->chars = 1;
	this->pixels = this->caretpos = this->caretxoffs = 0;
	this->markpos = this->markend = this->markxoffs = this->marklength = 0;
	this->UpdateStringIter();
}

/**
 * Insert a character to a textbuffer. If maxwidth of the Textbuf is zero,
 * we don't care about the visual-length but only about the physical
 * length of the string
 * @param key Character to be inserted
 * @return Return true on successful change of Textbuf, or false otherwise
 */
bool Textbuf::InsertChar(WChar key)
{
	uint16 len = (uint16)Utf8CharLen(key);
	if (this->len + len < this->capacity && this->chars + 1 <= this->max_chars) {
		memmove(this->buffer + this->caretpos + len, this->buffer + this->caretpos, this->len + 1 - this->caretpos);
		Utf8Encode(this->buffer + this->caretpos, key);
		this->chars++;
		this->len      += len;
		this->caretpos += len;

		this->UpdateStringIter();
		this->UpdateWidth();
		this->UpdateCaretPosition();
		this->UpdateMarkedText();
		return true;
	}
	return false;
}

/**
 * Insert a string into the text buffer. If maxwidth of the Textbuf is zero,
 * we don't care about the visual-length but only about the physical
 * length of the string.
 * @param str String to insert.
 * @param marked Replace the currently marked text with the new text.
 * @param caret Move the caret to this point in the insertion string.
 * @param insert_location Position at which to insert the string.
 * @param replacement_end Replace all characters from #insert_location up to this location with the new string.
 * @return True on successful change of Textbuf, or false otherwise.
 */
bool Textbuf::InsertString(const char *str, bool marked, const char *caret, const char *insert_location, const char *replacement_end)
{
	uint16 insertpos = (marked && this->marklength != 0) ? this->markpos : this->caretpos;
	if (insert_location != NULL) {
		insertpos = insert_location - this->buffer;
		if (insertpos > this->len + 1) return false;

		if (replacement_end != NULL) {
			this->DeleteText(insertpos, replacement_end - this->buffer, str == NULL);
		}
	} else {
		if (marked) this->DiscardMarkedText(str == NULL);
	}

	if (str == NULL) return false;

	uint16 bytes = 0, chars = 0;
	WChar c;
	for (const char *ptr = str; (c = Utf8Consume(&ptr)) != '\0';) {
		if (!IsValidChar(c, this->afilter)) break;

		byte len = Utf8CharLen(c);
		if (this->len   + bytes + len >= this->capacity) break;
		if (this->chars + chars + 1   > this->max_chars) break;

		bytes += len;
		chars++;

		/* Move caret if needed. */
		if (ptr == caret) this->caretpos = insertpos + bytes;
	}

	if (bytes == 0) return false;

	if (marked) {
		this->markpos = insertpos;
		this->markend = insertpos + bytes;
	}

	memmove(this->buffer + insertpos + bytes, this->buffer + insertpos, this->len + 1 - insertpos);
	memcpy(this->buffer + insertpos, str, bytes);

	this->len   += bytes;
	this->chars += chars;
	if (!marked && caret == NULL) this->caretpos += bytes;
	assert(this->len   <  this->capacity);
	assert(this->chars <= this->max_chars);
	this->buffer[this->len] = '\0'; // terminating zero

	this->UpdateStringIter();
	this->UpdateWidth();
	this->UpdateCaretPosition();
	this->UpdateMarkedText();

	return true;
}

/**
 * Insert a chunk of text from the clipboard onto the textbuffer. Get TEXT clipboard
 * and append this up to the maximum length (either absolute or screenlength). If maxlength
 * is zero, we don't care about the screenlength but only about the physical length of the string
 * @return true on successful change of Textbuf, or false otherwise
 */
bool Textbuf::InsertClipboard()
{
	char utf8_buf[512];

	if (!GetClipboardContents(utf8_buf, lengthof(utf8_buf))) return false;

	return this->InsertString(utf8_buf, false);
}

/**
 * Delete a part of the text.
 * @param from Start of the text to delete.
 * @param to End of the text to delete.
 * @param update Set to true if the internal state should be updated.
 */
void Textbuf::DeleteText(uint16 from, uint16 to, bool update)
{
	uint c = 0;
	const char *s = this->buffer + from;
	while (s < this->buffer + to) {
		Utf8Consume(&s);
		c++;
	}

	/* Strip marked characters from buffer. */
	memmove(this->buffer + from, this->buffer + to, this->len + 1 - to);
	this->len   -= to - from;
	this->chars -= c;

	/* Fixup caret if needed. */
	if (this->caretpos > from) {
		if (this->caretpos <= to) {
			this->caretpos = from;
		} else {
			this->caretpos -= to - from;
		}
	}

	if (update) {
		this->UpdateStringIter();
		this->UpdateCaretPosition();
		this->UpdateMarkedText();
	}
}

/**
 * Discard any marked text.
 * @param update Set to true if the internal state should be updated.
 */
void Textbuf::DiscardMarkedText(bool update)
{
	if (this->markend == 0) return;

	this->DeleteText(this->markpos, this->markend, update);
	this->markpos = this->markend = this->markxoffs = this->marklength = 0;
}

/** Update the character iter after the text has changed. */
void Textbuf::UpdateStringIter()
{
	this->char_iter->SetString(this->buffer);
	size_t pos = this->char_iter->SetCurPosition(this->caretpos);
	this->caretpos = pos == StringIterator::END ? 0 : (uint16)pos;
}

/** Update pixel width of the text. */
void Textbuf::UpdateWidth()
{
	this->pixels = GetStringBoundingBox(this->buffer, FS_NORMAL).width;
}

/** Update pixel position of the caret. */
void Textbuf::UpdateCaretPosition()
{
	this->caretxoffs = this->chars > 1 ? GetCharPosInString(this->buffer, this->buffer + this->caretpos, FS_NORMAL).x : 0;
}

/** Update pixel positions of the marked text area. */
void Textbuf::UpdateMarkedText()
{
	if (this->markend != 0) {
		this->markxoffs  = GetCharPosInString(this->buffer, this->buffer + this->markpos, FS_NORMAL).x;
		this->marklength = GetCharPosInString(this->buffer, this->buffer + this->markend, FS_NORMAL).x - this->markxoffs;
	} else {
		this->markxoffs = this->marklength = 0;
	}
}

/**
 * Handle text navigation with arrow keys left/right.
 * This defines where the caret will blink and the next character interaction will occur
 * @param keycode Direction in which navigation occurs (WKC_CTRL |) WKC_LEFT, (WKC_CTRL |) WKC_RIGHT, WKC_END, WKC_HOME
 * @return Return true on successful change of Textbuf, or false otherwise
 */
bool Textbuf::MovePos(uint16 keycode)
{
	switch (keycode) {
		case WKC_LEFT:
		case WKC_CTRL | WKC_LEFT: {
			if (this->caretpos == 0) break;

			size_t pos = this->char_iter->Prev(keycode & WKC_CTRL ? StringIterator::ITER_WORD : StringIterator::ITER_CHARACTER);
			if (pos == StringIterator::END) return true;

			this->caretpos = (uint16)pos;
			this->UpdateCaretPosition();
			return true;
		}

		case WKC_RIGHT:
		case WKC_CTRL | WKC_RIGHT: {
			if (this->caretpos >= this->length()) break;

			size_t pos = this->char_iter->Next(keycode & WKC_CTRL ? StringIterator::ITER_WORD : StringIterator::ITER_CHARACTER);
			if (pos == StringIterator::END) return true;

			this->caretpos = (uint16)pos;
			this->UpdateCaretPosition();
			return true;
		}

		case WKC_HOME:
			this->caretpos = 0;
			this->char_iter->SetCurPosition(this->caretpos);
			this->UpdateCaretPosition();
			return true;

		case WKC_END:
			this->caretpos = this->length();
			this->char_iter->SetCurPosition(this->caretpos);
			this->UpdateCaretPosition();
			return true;

		default:
			break;
	}

	return false;
}

/**
 * Initialize the textbuffer by supplying it the buffer to write into
 * and the maximum length of this buffer
 * @param buf the buffer that will be holding the data for input
 * @param max_bytes maximum size in bytes, including terminating '\0'
 * @param max_chars maximum size in chars, including terminating '\0'
 */
Textbuf::Textbuf(uint16 max_bytes, uint16 max_chars)
	: stringp(max_bytes),
	  max_chars(max_chars == UINT16_MAX ? max_bytes : max_chars)
{
	assert(max_bytes != 0);
	assert(max_chars != 0);

	this->char_iter = new StringIterator;

	this->afilter    = CS_ALPHANUMERAL;
	this->caret      = true;
	this->DeleteAll();
}

Textbuf::~Textbuf()
{
	delete this->char_iter;
}

/**
 * Render a string into the textbuffer.
 * @param string String
 */
void Textbuf::Assign(StringID string)
{
	GetString (this, string);
	this->UpdateSize();
}

/**
 * Copy a string into the textbuffer.
 * @param text Source.
 */
void Textbuf::Assign(const char *text)
{
	this->copy (text);
	this->UpdateSize();
}

/**
 * Print a formatted string into the textbuffer.
 */
void Textbuf::Print(const char *format, ...)
{
	va_list va;
	va_start(va, format);
	this->vfmt (format, va);
	va_end(va);
	this->UpdateSize();
}


/**
 * Update Textbuf type with its actual physical character and screenlength
 * Get the count of characters in the string as well as the width in pixels.
 * Useful when copying in a larger amount of text at once
 */
void Textbuf::UpdateSize()
{
	const char *buf = this->buffer;
	assert (strlen(buf) == this->len);
	assert (this->len < this->capacity);

	this->chars = 1; // terminating zero

	while (Utf8Consume(&buf) != '\0') {
		this->chars++;
	}
	assert(buf == this->buffer + this->len + 1);
	assert(this->chars <= this->max_chars);

	this->caretpos = this->len;
	this->UpdateStringIter();
	this->UpdateWidth();
	this->UpdateMarkedText();

	this->UpdateCaretPosition();
}

/**
 * Handle the flashing of the caret.
 * @return True if the caret state changes.
 */
bool Textbuf::HandleCaret()
{
	/* caret changed? */
	bool b = !!(_caret_timer & 0x20);

	if (b != this->caret) {
		this->caret = b;
		return true;
	}
	return false;
}

HandleKeyPressResult Textbuf::HandleKeyPress(WChar key, uint16 keycode)
{
	bool edited = false;

	switch (keycode) {
		case WKC_ESC: return HKPR_CANCEL;

		case WKC_RETURN: case WKC_NUM_ENTER: return HKPR_CONFIRM;

#ifdef WITH_COCOA
		case (WKC_META | 'V'):
#endif
		case (WKC_CTRL | 'V'):
			edited = this->InsertClipboard();
			break;

#ifdef WITH_COCOA
		case (WKC_META | 'U'):
#endif
		case (WKC_CTRL | 'U'):
			this->DeleteAll();
			edited = true;
			break;

		case WKC_BACKSPACE: case WKC_DELETE:
		case WKC_CTRL | WKC_BACKSPACE: case WKC_CTRL | WKC_DELETE:
			edited = this->DeleteChar(keycode);
			break;

		case WKC_LEFT: case WKC_RIGHT: case WKC_END: case WKC_HOME:
		case WKC_CTRL | WKC_LEFT: case WKC_CTRL | WKC_RIGHT:
			this->MovePos(keycode);
			break;

		default:
			if (IsValidChar(key, this->afilter)) {
				edited = this->InsertChar(key);
			} else {
				return HKPR_NOT_HANDLED;
			}
			break;
	}

	return edited ? HKPR_EDITING : HKPR_CURSOR;
}
