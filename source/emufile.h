/*
The MIT License

Copyright (C) 2009-2015 DeSmuME team

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

//don't use emufile for files bigger than 2GB! you have been warned! some day this will be fixed.

#ifndef EMUFILE_H
#define EMUFILE_H

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include <string>
#include <stdarg.h>

#include "emufile_types.h"

#ifdef HOST_WINDOWS 
#include <io.h>
#else
#include <unistd.h>
#include <sys/types.h>
#endif


class EMUFILE_MEMORY;

class EMUFILE {
protected:
	bool failbit;

public:
	EMUFILE()
		: failbit(false)
	{}


	//returns a new EMUFILE which is guranteed to be in memory. the EMUFILE you call this on may be deleted. use the returned EMUFILE in its place
	virtual EMUFILE* memwrap() = 0;

	virtual ~EMUFILE() {}
	
	static bool readAllBytes(std::vector<u8>* buf, const std::string& fname);

	bool fail(bool unset=false) { bool ret = failbit; if(unset) unfail(); return ret; }
	void unfail() { failbit=false; }

	bool eof() { return size()==ftell(); }

	size_t fread(const void *ptr, size_t bytes){
		return _fread(ptr,bytes);
	}

	void unget() { fseek(-1,SEEK_CUR); }

	//virtuals
public:

	virtual FILE *get_fp() = 0;

	virtual int fprintf(const char *format, ...) = 0;

	virtual int fgetc() = 0;
	virtual int fputc(int c) = 0;

	virtual size_t _fread(const void *ptr, size_t bytes) = 0;
	virtual size_t fwrite(const void *ptr, size_t bytes) = 0;

	void write64le(u64* val);
	void write64le(u64 val);
	size_t read64le(u64* val);
	u64 read64le();
	void write32le(u32* val);
	void write32le(s32* val) { write32le((u32*)val); }
	void write32le(u32 val);
	size_t read32le(u32* val);
	size_t read32le(s32* val);
	u32 read32le();
	void write16le(u16* val);
	void write16le(s16* val) { write16le((u16*)val); }
	void write16le(u16 val);
	size_t read16le(s16* Bufo);
	size_t read16le(u16* val);
	u16 read16le();
	void write8le(u8* val);
	void write8le(u8 val);
	size_t read8le(u8* val);
	u8 read8le();
	void writedouble(double* val);
	void writedouble(double val);
	double readdouble();
	size_t readdouble(double* val);

	virtual int fseek(int offset, int origin) = 0;

	virtual int ftell() = 0;
	virtual int size() = 0;
	virtual void fflush() = 0;
    
	virtual void truncate(s32 length) = 0;

	void writeMemoryStream(EMUFILE_MEMORY* ms);
	void readMemoryStream(EMUFILE_MEMORY* ms);

};

//todo - handle read-only specially?
class EMUFILE_MEMORY : public EMUFILE { 
protected:
	std::vector<u8> *vec;
	bool ownvec;
	s32 pos, len;

	void reserve(u32 amt) {
		if(vec->size() < amt)
			vec->resize(amt);
	}

public:

	EMUFILE_MEMORY(std::vector<u8> *underlying) : vec(underlying), ownvec(false), pos(0), len((s32)underlying->size()) { }
	EMUFILE_MEMORY(u32 preallocate) : vec(new std::vector<u8>()), ownvec(true), pos(0), len(0) { 
		vec->resize(preallocate);
		len = preallocate;
	}
	EMUFILE_MEMORY() : vec(new std::vector<u8>()), ownvec(true), pos(0), len(0) { vec->reserve(1024); }
	EMUFILE_MEMORY(void* buf, s32 size) : vec(new std::vector<u8>()), ownvec(true), pos(0), len(size) { 
		vec->resize(size);
		if(size != 0)
			memcpy(&vec->front(),buf,size);
	}

	~EMUFILE_MEMORY() {
		if(ownvec) delete vec;
	}

	virtual EMUFILE* memwrap();

	virtual void truncate(s32 length)
	{
		vec->resize(length);
		len = length;
		if(pos>length) pos=length;
	}

	u8* buf() { 
		if(size()==0) reserve(1);
		return &(*vec)[0];
	}

	std::vector<u8>* get_vec() const { return vec; };

	virtual FILE *get_fp() { return NULL; }

	virtual int fprintf(const char *format, ...) {
		va_list argptr;
		va_start(argptr, format);

		//we dont generate straight into the buffer because it will null terminate (one more byte than we want)
		int amt = vsnprintf(0,0,format,argptr);
		//int amt = _vsnprintf(0,0,format,argptr);
		
		char* tempbuf = new char[amt+1];

		va_end(argptr);
		va_start(argptr, format);
		vsprintf(tempbuf,format,argptr);
		
		fwrite(tempbuf,amt);
		delete[] tempbuf;

		va_end(argptr);
		return amt;
	};

	virtual int fgetc() {
		u8 temp;

		//need an optimized codepath
		//if(_fread(&temp,1) != 1)
		//	return EOF;
		//else return temp;
		u32 remain = len-pos;
		if(remain<1) {
			failbit = true;
			return -1;
		}
		temp = buf()[pos];
		pos++;
		return temp;
	}
	virtual int fputc(int c) {
		u8 temp = (u8)c;
		//TODO
		//if(fwrite(&temp,1)!=1) return EOF;
		fwrite(&temp,1);

		return 0;
	}

	virtual size_t _fread(const void *ptr, size_t bytes);
	virtual size_t fwrite(const void *ptr, size_t bytes){
		reserve(pos+(s32)bytes);
		memcpy(buf()+pos,ptr,bytes);
		pos += (s32)bytes;
		len = std::max(pos,len);
		
		return bytes;
	}

	virtual int fseek(int offset, int origin){ 
		//work differently for read-only...?
		switch(origin) {
			case SEEK_SET:
				pos = offset;
				break;
			case SEEK_CUR:
				pos += offset;
				break;
			case SEEK_END:
				pos = size()+offset;
				break;
			default:
				assert(false);
		}
		reserve(pos);
		return 0;
	}

	virtual int ftell() {
		return pos;
	}

	virtual void fflush() {}

	void trim()
	{
		vec->resize(len);
	}

	virtual int size() { return (int)len; }
};

class EMUFILE_FILE : public EMUFILE { 
protected:
	FILE* fp;
	std::string fname;
	char mode[16];
	long mFilePosition;
	bool mPositionCacheEnabled;
	
	enum eCondition
	{
		eCondition_Clean,
		eCondition_Unknown,
		eCondition_Read,
		eCondition_Write
	} mCondition;

private:
	void open(const char* fname, const char* mode)
	{
		mPositionCacheEnabled = false;
		mCondition = eCondition_Clean;
		fp = fopen(fname,mode);
		if(!fp)
			failbit = true;
		this->fname = fname;
		strcpy(this->mode,mode);
	}

public:

	EMUFILE_FILE(const std::string& fname, const char* mode) { open(fname.c_str(),mode); }
	EMUFILE_FILE(const char* fname, const char* mode) { open(fname,mode); }

	void EnablePositionCache();

	virtual ~EMUFILE_FILE() {
		if(NULL != fp)
			fclose(fp);
	}

	virtual FILE *get_fp() {
		return fp; 
	}

	virtual EMUFILE* memwrap();

	bool is_open() { return fp != NULL; }

	void DemandCondition(eCondition cond);

	virtual void truncate(s32 length);

	virtual int fprintf(const char *format, ...) {
		va_list argptr;
		va_start(argptr, format);
		int ret = ::vfprintf(fp, format, argptr);
		va_end(argptr);
		return ret;
	};

	virtual int fgetc() {
		return ::fgetc(fp);
	}
	virtual int fputc(int c) {
		return ::fputc(c, fp);
	}

	virtual size_t _fread(const void *ptr, size_t bytes);
	virtual size_t fwrite(const void *ptr, size_t bytes);

	virtual int fseek(int offset, int origin);

	virtual int ftell();

	virtual int size() { 
		int oldpos = ftell();
		fseek(0,SEEK_END);
		int len = ftell();
		fseek(oldpos,SEEK_SET);
		return len;
	}

	virtual void fflush() {
		::fflush(fp);
	}

};

#endif
