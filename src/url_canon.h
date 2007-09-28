// Copyright 2007, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#ifndef GOOGLEURL_SRC_URL_CANON_H__
#define GOOGLEURL_SRC_URL_CANON_H__

#include <memory.h>
#include <stdlib.h>

#include "googleurl/src/url_parse.h"

namespace url_canon {

// Canonicalizer output -------------------------------------------------------

// Base class for the canonicalizer output, this maintains a buffer and
// supports simple resizing and append operations on it.
//
// It is VERY IMPORTANT that no virtual function calls be made on the common
// code path. We only have two virtual function calls, the destructor and a
// resize function that is called when the existing buffer is not big enough.
// The derived class is then in charge of setting up our buffer which we will
// manage.
template<typename T>
class CanonOutputT {
 public:
  CanonOutputT() : buffer_(NULL), buffer_len_(0), cur_len_(0) {
  }
  virtual ~CanonOutputT() {
  }

  // Implemented to resize the buffer. This function should update the buffer
  // pointer to point to the new buffer, and any old data up to |cur_len_| in
  // the buffer must be copied over.
  //
  // The new size |sz| must be larger than buffer_len_.
  virtual void Resize(int sz) = 0;

  // Accessor for returning a character at a given position. The input offset
  // must be in the valid range.
  inline char at(int offset) const {
    return buffer_[offset];
  }

  // Sets the character at the given position. The given position MUST be less
  // than the length().
  inline void set(int offset, int ch) {
    buffer_[offset] = ch;
  }

  // Returns the number of characters currently in the buffer.
  inline int length() const {
    return cur_len_;
  }

  // Returns the current capacity of the buffer. The length() is the number of
  // characters that have been declared to be written, but the capacity() is
  // the number that can be written without reallocation. If the caller must
  // write many characters at once, it can make sure there is enough capacity,
  // write the data, then use set_size() to declare the new length().
  int capacity() const {
    return buffer_len_;
  }

  // Called by the user of this class to get the output. The output will NOT
  // be NULL-terminated. Call length() to get the
  // length.
  const T* data() const {
    return buffer_;
  }
  T* data() {
    return buffer_;
  }

  // Shortens the URL to the new length. Used for "backing up" when processing
  // relative paths. This can also be used if an external function writes a lot
  // of data to the buffer (when using the "Raw" version below) beyond the end,
  // to declare the new length.
  //
  // This MUST NOT be used to expand the size of the buffer beyond capacity().
  void set_length(int new_len) {
    cur_len_ = new_len;
  }

  // This is the most performance critical function, since it is called for
  // every character.
  void push_back(T ch) {
    // In VC2005, putting this common case first speeds up execution
    // dramatically because this branch is predicted as taken.
    if (cur_len_ < buffer_len_) {
      buffer_[cur_len_] = ch;
      cur_len_++;
      return;
    }

    // Grow the buffer to hold at least one more item. Hopefully we won't have
    // to do this very often.
    if (!Grow(1))
      return;

    // Actually do the insertion.
    buffer_[cur_len_] = ch;
    cur_len_++;
  }

  // Appends the given string to the output.
  void Append(const T* str, int str_len) {
    if (cur_len_ + str_len > buffer_len_) {
      if (!Grow(cur_len_ + str_len - buffer_len_))
        return;
    }
    for (int i = 0; i < str_len; i++)
      buffer_[cur_len_ + i] = str[i];
    cur_len_ += str_len;
  }

 protected:
  // Grows the given buffer so that it can fit at least |min_additional|
  // characters. Returns true if the buffer could be resized, false on OOM.
  bool Grow(int min_additional) {
    int new_len = buffer_len_;
    do {
      if (new_len >= (1 << 30))  // Prevent overflow below.
        return false;
      new_len *= 2;
    } while (new_len < buffer_len_ + min_additional);
    Resize(new_len);
    return true;
  }

  T* buffer_;
  int buffer_len_;

  // Used characters in the buffer.
  int cur_len_;
};

// Simple implementation of the CanonOutput using new[]. This class
// also supports a static buffer so if it is allocated on the stack, most
// URLs can be canonicalized with no heap allocations.
template<typename T, int fixed_capacity = 1024>
class RawCanonOutputT : public CanonOutputT<T> {
 public:
  RawCanonOutputT() : CanonOutputT<T>() {
    buffer_ = fixed_buffer_;
    buffer_len_ = fixed_capacity;
  }
  virtual ~RawCanonOutputT() {
    if (buffer_ != fixed_buffer_)
      delete[] buffer_;
  }

  virtual void Resize(int sz) {
    T* new_buf = new T[sz];
    memcpy(new_buf, buffer_,
           sizeof(T) * (cur_len_ < sz ? cur_len_ : sz));
    if (buffer_ != fixed_buffer_)
      delete[] buffer_;
    buffer_ = new_buf;
    buffer_len_ = sz;
  }

 protected:
  T fixed_buffer_[fixed_capacity];
};

// Normally, all canonicalization output is in narrow characters. We support
// the templates so it can also be used internally if a wide buffer is
// required.
typedef CanonOutputT<char> CanonOutput;
typedef CanonOutputT<wchar_t> CanonOutputW;

template<int fixed_capacity>
class RawCanonOutput : public RawCanonOutputT<char, fixed_capacity> {};
template<int fixed_capacity>
class RawCanonOutputW : public RawCanonOutputT<wchar_t, fixed_capacity> {};

// Character set converter ----------------------------------------------------
//
// Converts query strings into a custom encoding. The embedder can supply an
// implementation of this class to interface with their own character set
// conversion libraries.
//
// Embedders will want to see the unit test for the ICU version.

class CharsetConverter {
 public:
  CharsetConverter() {}
  virtual ~CharsetConverter() {}

  // Converts the given input string from UTF-16 to whatever output format the
  // converter supports. This is used only for the query encoding conversion,
  // which does not fail. Instead, the converter should insert "invalid
  // character" characters in the output for invalid sequences, and do the
  // best it can.
  //
  // If the input contains a character not representable in the output
  // character set, the converter should append the HTML entity sequence in
  // decimal, (such as "&#20320;") with escaping of the ampersand, number
  // sign, and semicolon (in the previous example it would be
  // "%26%2320320%3B"). This rule is based on what IE does in this situation.
  virtual void ConvertFromUTF16(const wchar_t* input,
                                int input_len,
                                CanonOutput* output) = 0;
};

// IDN ------------------------------------------------------------------------

// Converts the Unicode input representing a hostname to ASCII using IDN rules.
// The output must fall in the ASCII range, but will be encoded in UTF-16.
//
// On success, the output will be filled with the ASCII host name and it will
// return true. Unlike most other canonicalization functions, this assumes that
// the output is empty. The beginning of the host will be at offset 0, and
// the length of the output will be set to the length of the new host name.
//
// On error, returns false. The output in this case is undefined.
bool IDNToASCII(const wchar_t* src, int src_len, CanonOutputW* output);

// Piece-by-piece canonicalizers ----------------------------------------------
//
// These individual canonicalizers append the canonicalized versions of the
// corresponding URL component to the given std::string. The spec and the
// previously-identified range of that component are the input. The range of
// the canonicalized component will be written to the output component.
//
// These functions all append to the output so they can be chained. Make sure
// the output is empty when you start.
//
// These functions returns boolean values indicating success. On failure, they
// will attempt to write something reasonable to the output so that, if
// displayed to the user, they will recognise it as something that's messed up.
// Nothing more should ever be done with these invalid URLs, however.

// Scheme: Appends the scheme and colon to the URL. The output component will
// indicate the range of characters up to but not including the colon.
//
// Canonical URLs always have a scheme. If the scheme is not present in the
// input, this will just write the colon to indicate an empty scheme. Does not
// append slashes which will be needed before any authority components for most
// URLs.
//
// The 8-bit version requires UTF-8 encoding.
bool CanonicalizeScheme(const char* spec,
                        const url_parse::Component& scheme,
                        CanonOutput* output,
                        url_parse::Component* out_scheme);
bool CanonicalizeScheme(const wchar_t* spec,
                        const url_parse::Component& scheme,
                        CanonOutput* output,
                        url_parse::Component* out_scheme);

// User info: username/password. If present, this will add the delimiters so
// the output will be "<username>:<password>@" or "<username>@". Empty
// username/password pairs, or empty passwords, will get converted to
// nonexistant in the canonical version.
//
// The components for the username and password refer to ranges in the
// respective source strings. Usually, these will be the same string, which
// is legal as long as the two components don't overlap.
//
// The 8-bit version requires UTF-8 encoding.
bool CanonicalizeUserInfo(const char* username_source,
                          const url_parse::Component& username,
                          const char* password_source,
                          const url_parse::Component& password,
                          CanonOutput* output,
                          url_parse::Component* out_username,
                          url_parse::Component* out_password);
bool CanonicalizeUserInfo(const wchar_t* username_source,
                          const url_parse::Component& username,
                          const wchar_t* password_source,
                          const url_parse::Component& password,
                          CanonOutput* output,
                          url_parse::Component* out_username,
                          url_parse::Component* out_password);

// Host.
//
// The 8-bit version requires UTF-8 encoding.
bool CanonicalizeHost(const char* spec,
                      const url_parse::Component& host,
                      CanonOutput* output,
                      url_parse::Component* out_host);
bool CanonicalizeHost(const wchar_t* spec,
                      const url_parse::Component& host,
                      CanonOutput* output,
                      url_parse::Component* out_host);

// Returns true if ch is valid character for a host.
bool IsValidHostCharacter(wchar_t ch);


// IP addresses.
//
// Tries to interpret the given host name as an IP address. If it is an IP
// address, it will canonicalize it as such, appending it to |output| and
// identifying the added regions in |*out_host|, and will return true. If it
// is not an IP address, it will do nothing and will return false. This means
// that the host name should be treated as a non-IP address and resolved using
// DNS like most names.
//
// This is called AUTOMATICALLY from the host canonicalizer, which ensures that
// the input is unescaped and name-prepped, etc. It should not normally be
// necessary or wise to call this directly, other than to check if a given
// canonical hostname is an IP address.
bool CanonicalizeIPAddress(const char* spec,
                           const url_parse::Component& host,
                           CanonOutput* output,
                           url_parse::Component* out_host);
bool CanonicalizeIPAddress(const wchar_t* spec,
                           const url_parse::Component& host,
                           CanonOutput* output,
                           url_parse::Component* out_host);

// Port: this function will add the colon for the port if a port is present.
//
// The 8-bit version requires UTF-8 encoding.
bool CanonicalizePort(const char* spec,
                      const url_parse::Component& port,
                      int default_port_for_scheme,
                      CanonOutput* output,
                      url_parse::Component* out_port);
bool CanonicalizePort(const wchar_t* spec,
                      const url_parse::Component& port,
                      int default_port_for_scheme,
                      CanonOutput* output,
                      url_parse::Component* out_port);

// Path. If the input does not begin in a slash (including if the input is
// empty), we'll prepend a slash to the path to make it canonical.
//
// The 8-bit version assumes UTF-8 encoding, but does not verify the validity
// of the UTF-8 (i.e., you can have invalid UTF-8 sequences, invalid
// characters, etc.). Normally, URLs will come in as UTF-16, so this isn't
// an issue. Somebody giving us an 8-bit path is responsible for generating
// the path that the server expects (we'll escape high-bit characters), so
// if something is invalid, it's their problem.
bool CanonicalizePath(const char* spec,
                      const url_parse::Component& path,
                      CanonOutput* output,
                      url_parse::Component* out_path);
bool CanonicalizePath(const wchar_t* spec,
                      const url_parse::Component& path,
                      CanonOutput* output,
                      url_parse::Component* out_path);

// Canonicalizes the input as a file path. This is like CanonicalizePath except
// that it also handles Windows drive specs. For example, the path can begin
// with "c|\" and it will get properly canonicalized to "C:/".
// The string will be appended to |*output| and |*out_path| will be updated.
//
// The 8-bit version requires UTF-8 encoding.
bool FileCanonicalizePath(const char* spec,
                          const url_parse::Component& path,
                          CanonOutput* output,
                          url_parse::Component* out_path);
bool FileCanonicalizePath(const wchar_t* spec,
                          const url_parse::Component& path,
                          CanonOutput* output,
                          url_parse::Component* out_path);

// Query: Prepends the ? if needed.
//
// The 8-bit version requires the input to be UTF-8 encoding. Incorrectly
// encoded characters (in UTF-8 or UTF-16) will be replaced with the Unicode
// "invalid character." This function can not fail, we always just try to do
// our best for crazy input here since web pages can set it themselves.
//
// This will convert the given input into the output encoding that the given
// character set converter object provides. The converter will only be called
// if necessary, for ASCII input, no conversions are necessary.
//
// The converter can be NULL. In this case, the output encoding will be UTF-8.
void CanonicalizeQuery(const char* spec,
                       const url_parse::Component& query,
                       CharsetConverter* converter,
                       CanonOutput* output,
                       url_parse::Component* out_query);
void CanonicalizeQuery(const wchar_t* spec,
                       const url_parse::Component& query,
                       CharsetConverter* converter,
                       CanonOutput* output,
                       url_parse::Component* out_query);

// Ref: Prepends the # if needed. The output will be UTF-8 (this is the only
// canonicalizer that does not produce ASCII output). The output is
// guaranteed to be valid UTF-8.
//
// The only way this function will fail is if the input is invalid
// UTF-8/UTF-16. In this case, we'll use the "Unicode replacement character"
// for the confusing bits and copy the rest. The application will probably not
// want to treat a failure converting the ref as a failure canonicalizing the
// URL, since the page can probably still be loaded, just not scrolled
// properly.
bool CanonicalizeRef(const char* spec,
                     const url_parse::Component& path,
                     CanonOutput* output,
                     url_parse::Component* out_path);
bool CanonicalizeRef(const wchar_t* spec,
                     const url_parse::Component& path,
                     CanonOutput* output,
                     url_parse::Component* out_path);

// Full canonicalizer ---------------------------------------------------------
//
// These functions replace any string contents, rather than append as above.
// See the above piece-by-piece functions for information specific to
// canonicalizing individual components.
//
// The output will be ASCII except the reference fragment, which may be UTF-8.
//
// The 8-bit versions require UTF-8 encoding.

// Use for standard URLs with authorities and paths.
bool CanonicalizeStandardURL(const char* spec,
                             int spec_len,
                             const url_parse::Parsed& parsed,
                             CanonOutput* output,
                             url_parse::Parsed* new_parsed);
bool CanonicalizeStandardURL(const wchar_t* spec,
                             int spec_len,
                             const url_parse::Parsed& parsed,
                             CanonOutput* output,
                             url_parse::Parsed* new_parsed);

// Use for file URLs.
bool CanonicalizeFileURL(const char* spec,
                         int spec_len,
                         const url_parse::Parsed& parsed,
                         CanonOutput* output,
                         url_parse::Parsed* new_parsed);
bool CanonicalizeFileURL(const wchar_t* spec,
                         int spec_len,
                         const url_parse::Parsed& parsed,
                         CanonOutput* output,
                         url_parse::Parsed* new_parsed);

// Use for path URLs such as javascript. This does not modify the path in any
// way, for example, by escaping it.
bool CanonicalizePathURL(const char* spec,
                         int spec_len,
                         const url_parse::Parsed& parsed,
                         CanonOutput* output,
                         url_parse::Parsed* new_parsed);
bool CanonicalizePathURL(const wchar_t* spec,
                         int spec_len,
                         const url_parse::Parsed& parsed,
                         CanonOutput* output,
                         url_parse::Parsed* new_parsed);

// Part replacer --------------------------------------------------------------

// Structure for overriding components in a URL. Pointers to each replacement
// are specified here. Internally, the canonicalizer also uses this structure
// to track the source of each of the |Parsed| components. This allows the
// replacement code and the canonicalization code to be shared because the
// source for each component is always general. Internally in the canonicalizer
// we can not handle NULLs. The NULLs are substituded in the Replace...()
// functions before going to the canonicalizer.
//
// Supplying a NULL means that the corresponding component should be unchanged.
// Supplying an empty string means that element should be deleted.
//
// An empty string will delete that component. For the types of components that
// can be either empty or nonexistant (think the difference between not having
// a question mark and a question mark with nothing following it), this
// function will assume nonexistant when given an empty input string.
//
// Note: if the base URL is non-standard (i.e. a "path" like javascript:) then
// only the scheme and path can be set. If the base URL is a file, then only
// the host, path, param, query, and ref can be set.
//
// The 8-bit version requires UTF-8 encoding.
template<typename CHAR>
struct URLComponentSource {
  // Constructor normally used by callers wishing to replace components. This
  // will make them all NULL, which is no replacement. The caller would then
  // override the compoents they want to replace.
  URLComponentSource()
      : scheme(NULL),
        username(NULL),
        password(NULL),
        host(NULL), 
        port(NULL),
        path(NULL),
        query(NULL),
        ref(NULL) {
  }

  // Constructor normally used internally to initialize all the components to
  // point to the same spec.
  URLComponentSource(const CHAR* default_value)
      : scheme(default_value),
        username(default_value),
        password(default_value),
        host(default_value),
        port(default_value),
        path(default_value),
        query(default_value),
        ref(default_value) {
  }

  const CHAR* scheme;
  const CHAR* username;
  const CHAR* password;
  const CHAR* host;
  const CHAR* port;
  const CHAR* path;
  const CHAR* query;
  const CHAR* ref;
};

// The base must be a narrow canonical URL.
bool ReplaceStandardURL(const char* base,
                        int base_len,
                        const url_parse::Parsed& base_parsed,
                        const URLComponentSource<char>& replacements,
                        CanonOutput* output,
                        url_parse::Parsed* new_parsed);

// Replacing some parts of a file URL is not permitted. Everything except
// the host, path, query, and ref will be ignored.
bool ReplaceFileURL(const char* base,
                    int base_len,
                    const url_parse::Parsed& base_parsed,
                    const URLComponentSource<char>& replacements,
                    CanonOutput* output,
                    url_parse::Parsed* new_parsed);

// Path URLs can only have the scheme and path replaced. All other components
// will be ignored.
bool ReplacePathURL(const char* base,
                    int base_len,
                    const url_parse::Parsed& base_parsed,
                    const URLComponentSource<char>& replacements,
                    CanonOutput* output,
                    url_parse::Parsed* new_parsed);

// Relative URL ---------------------------------------------------------------

// Given an input URL or URL fragment |fragment|, determines if it is a
// relative or absolute URL and places the result into |*is_relative|. If it is
// relative, the relevant portion of the URL will be placed into
// |*relative_component| (there may have been trimmed whitespace, for example).
// This value is passed to ResolveRelativeURL. If the input is not relative,
// this value is UNDEFINED (it may be changed by the functin).
//
// Returns true on success (we successfully determined the URL is relative or
// not). Failure means that the combination of URLs doesn't make any sense.
//
// The base URL should always be canonical, therefore is ASCII.
bool IsRelativeURL(const char* base,
                   const url_parse::Parsed& base_parsed,
                   const char* fragment,
                   int fragment_len,
                   bool is_base_hierarchical,
                   bool* is_relative,
                   url_parse::Component* relative_component);
bool IsRelativeURL(const char* base,
                   const url_parse::Parsed& base_parsed,
                   const wchar_t* fragment,
                   int fragment_len,
                   bool is_base_hierarchical,
                   bool* is_relative,
                   url_parse::Component* relative_component);

// Given a canonical parsed source URL, a URL fragment known to be relative,
// and the identified relevant portion of the relative URL (computed by
// IsRelativeURL), this produces a new parsed canonical URL in |output| and
// |out_parsed|.
//
// It also requires a flag indicating whether the base URL is a file: URL
// which triggers additional logic.
//
// The base URL should be canonical and have a host (may be empty for file
// URLs) and a path. If it doesn't have these, we can't resolve relative
// URLs off of it and will return the base as the output with an error flag.
// Becausee it is canonical is should also be ASCII.
//
// Returns true on success. On failure, the output will be "something
// reasonable" that will be consistent and valid, just probably not what
// was intended by the web page author or caller.
bool ResolveRelativeURL(const char* base_url,
                        const url_parse::Parsed& base_parsed,
                        bool base_is_file,
                        const char* relative_url,
                        const url_parse::Component& relative_component,
                        CanonOutput* output,
                        url_parse::Parsed* out_parsed);
bool ResolveRelativeURL(const char* base_url,
                        const url_parse::Parsed& base_parsed,
                        bool base_is_file,
                        const wchar_t* relative_url,
                        const url_parse::Component& relative_component,
                        CanonOutput* output,
                        url_parse::Parsed* out_parsed);

}  // namespace url_canon

#endif  // GOOGLEURL_SRC_URL_CANON_H__
