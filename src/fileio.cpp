/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file fileio.cpp Standard In/Out file operations */

#include "stdafx.h"
#include "fileio_func.h"
#include "debug.h"
#include "fios.h"
#include "string.h"
#include "tar_type.h"
#ifdef WIN32
#include <windows.h>
# define access _taccess
#elif defined(__HAIKU__)
#include <Path.h>
#include <storage/FindDirectory.h>
#else
#include <unistd.h>
#include <pwd.h>
#endif
#include <sys/stat.h>
#include <algorithm>

#ifdef WITH_XDG_BASEDIR
#include "basedir.h"
#endif

/** Size of the #Fio data buffer. */
#define FIO_BUFFER_SIZE 512

/** Structure for keeping several open files with just one data buffer. */
struct Fio {
	byte *buffer, *buffer_end;             ///< position pointer in local buffer and last valid byte of buffer
	size_t pos;                            ///< current (system) position in file
	FILE *cur_fh;                          ///< current file handle
	const char *filename;                  ///< current filename
	FILE *handles[MAX_FILE_SLOTS];         ///< array of file handles we can have open
	byte buffer_start[FIO_BUFFER_SIZE];    ///< local buffer when read from file
	const char *filenames[MAX_FILE_SLOTS]; ///< array of filenames we (should) have open
	char *shortnames[MAX_FILE_SLOTS];      ///< array of short names for spriteloader's use
#if defined(LIMITED_FDS)
	uint open_handles;                     ///< current amount of open handles
	uint usage_count[MAX_FILE_SLOTS];      ///< count how many times this file has been opened
#endif /* LIMITED_FDS */
};

static Fio _fio; ///< #Fio instance.

/** Whether the working directory should be scanned. */
static bool _do_scan_working_directory = true;

extern char *_config_file;
extern char *_highscore_file;

/**
 * Get position in the current file.
 * @return Position in the file.
 */
size_t FioGetPos()
{
	return _fio.pos + (_fio.buffer - _fio.buffer_end);
}

/**
 * Get the filename associated with a slot.
 * @param slot Index of queried file.
 * @return Name of the file.
 */
const char *FioGetFilename(uint8 slot)
{
	return _fio.shortnames[slot];
}

/**
 * Seek in the current file.
 * @param pos New position.
 * @param mode Type of seek (\c SEEK_CUR means \a pos is relative to current position, \c SEEK_SET means \a pos is absolute).
 */
void FioSeekTo(size_t pos, int mode)
{
	if (mode == SEEK_CUR) pos += FioGetPos();
	_fio.buffer = _fio.buffer_end = _fio.buffer_start + FIO_BUFFER_SIZE;
	_fio.pos = pos;
	if (fseek(_fio.cur_fh, _fio.pos, SEEK_SET) < 0) {
		DEBUG(misc, 0, "Seeking in %s failed", _fio.filename);
	}
}

#if defined(LIMITED_FDS)
static void FioRestoreFile(int slot)
{
	/* Do we still have the file open, or should we reopen it? */
	if (_fio.handles[slot] == NULL) {
		DEBUG(misc, 6, "Restoring file '%s' in slot '%d' from disk", _fio.filenames[slot], slot);
		FioOpenFile(slot, _fio.filenames[slot]);
	}
	_fio.usage_count[slot]++;
}
#endif /* LIMITED_FDS */

/**
 * Switch to a different file and seek to a position.
 * @param slot Slot number of the new file.
 * @param pos New absolute position in the new file.
 */
void FioSeekToFile(uint8 slot, size_t pos)
{
	FILE *f;
#if defined(LIMITED_FDS)
	/* Make sure we have this file open */
	FioRestoreFile(slot);
#endif /* LIMITED_FDS */
	f = _fio.handles[slot];
	assert(f != NULL);
	_fio.cur_fh = f;
	_fio.filename = _fio.filenames[slot];
	FioSeekTo(pos, SEEK_SET);
}

/**
 * Read a byte from the file.
 * @return Read byte.
 */
byte FioReadByte()
{
	if (_fio.buffer == _fio.buffer_end) {
		_fio.buffer = _fio.buffer_start;
		size_t size = fread(_fio.buffer, 1, FIO_BUFFER_SIZE, _fio.cur_fh);
		_fio.pos += size;
		_fio.buffer_end = _fio.buffer_start + size;

		if (size == 0) return 0;
	}
	return *_fio.buffer++;
}

/**
 * Skip \a n bytes ahead in the file.
 * @param n Number of bytes to skip reading.
 */
void FioSkipBytes(int n)
{
	for (;;) {
		int m = min(_fio.buffer_end - _fio.buffer, n);
		_fio.buffer += m;
		n -= m;
		if (n == 0) break;
		FioReadByte();
		n--;
	}
}

/**
 * Read a word (16 bits) from the file (in low endian format).
 * @return Read word.
 */
uint16 FioReadWord()
{
	byte b = FioReadByte();
	return (FioReadByte() << 8) | b;
}

/**
 * Read a double word (32 bits) from the file (in low endian format).
 * @return Read word.
 */
uint32 FioReadDword()
{
	uint b = FioReadWord();
	return (FioReadWord() << 16) | b;
}

/**
 * Read a block.
 * @param ptr Destination buffer.
 * @param size Number of bytes to read.
 */
void FioReadBlock(void *ptr, size_t size)
{
	FioSeekTo(FioGetPos(), SEEK_SET);
	_fio.pos += fread(ptr, 1, size, _fio.cur_fh);
}

/**
 * Close the file at the given slot number.
 * @param slot File index to close.
 */
static inline void FioCloseFile(int slot)
{
	if (_fio.handles[slot] != NULL) {
		fclose(_fio.handles[slot]);

		free(_fio.shortnames[slot]);
		_fio.shortnames[slot] = NULL;

		_fio.handles[slot] = NULL;
#if defined(LIMITED_FDS)
		_fio.open_handles--;
#endif /* LIMITED_FDS */
	}
}

/** Close all slotted open files. */
void FioCloseAll()
{
	for (int i = 0; i != lengthof(_fio.handles); i++) {
		FioCloseFile(i);
	}
}

#if defined(LIMITED_FDS)
static void FioFreeHandle()
{
	/* If we are about to open a file that will exceed the limit, close a file */
	if (_fio.open_handles + 1 == LIMITED_FDS) {
		uint i, count;
		int slot;

		count = UINT_MAX;
		slot = -1;
		/* Find the file that is used the least */
		for (i = 0; i < lengthof(_fio.handles); i++) {
			if (_fio.handles[i] != NULL && _fio.usage_count[i] < count) {
				count = _fio.usage_count[i];
				slot  = i;
			}
		}
		assert(slot != -1);
		DEBUG(misc, 6, "Closing filehandler '%s' in slot '%d' because of fd-limit", _fio.filenames[slot], slot);
		FioCloseFile(slot);
	}
}
#endif /* LIMITED_FDS */

/**
 * Open a slotted file.
 * @param slot Index to assign.
 * @param filename Name of the file at the disk.
 * @param subdir The sub directory to search this file in.
 */
void FioOpenFile(int slot, const char *filename, Subdirectory subdir)
{
	FILE *f;

#if defined(LIMITED_FDS)
	FioFreeHandle();
#endif /* LIMITED_FDS */
	f = FioFOpenFile(filename, "rb", subdir);
	if (f == NULL) usererror("Cannot open file '%s'", filename);
	long pos = ftell(f);
	if (pos < 0) usererror("Cannot read file '%s'", filename);

	FioCloseFile(slot); // if file was opened before, close it
	_fio.handles[slot] = f;
	_fio.filenames[slot] = filename;

	/* Store the filename without path and extension */
	const char *t = strrchr(filename, PATHSEPCHAR);
	_fio.shortnames[slot] = xstrdup(t == NULL ? filename : t);
	char *t2 = strrchr(_fio.shortnames[slot], '.');
	if (t2 != NULL) *t2 = '\0';
	strtolower(_fio.shortnames[slot]);

#if defined(LIMITED_FDS)
	_fio.usage_count[slot] = 0;
	_fio.open_handles++;
#endif /* LIMITED_FDS */
	FioSeekToFile(slot, (uint32)pos);
}

static const char * const _subdirs[] = {
	"",
	"save" PATHSEP,
	"save" PATHSEP "autosave" PATHSEP,
	"scenario" PATHSEP,
	"scenario" PATHSEP "heightmap" PATHSEP,
	"gm" PATHSEP,
	"data" PATHSEP,
	"baseset" PATHSEP,
	"newgrf" PATHSEP,
	"lang" PATHSEP,
	"ai" PATHSEP,
	"ai" PATHSEP "library" PATHSEP,
	"game" PATHSEP,
	"game" PATHSEP "library" PATHSEP,
	"screenshot" PATHSEP,
};
assert_compile(lengthof(_subdirs) == NUM_SUBDIRS);

const char *_searchpaths[NUM_SEARCHPATHS];

TarCache TarCache::cache [NUM_SUBDIRS];

/**
 * Check whether the given file exists
 * @param filename the file to try for existence.
 * @param subdir the subdirectory to look in
 * @return true if and only if the file can be opened
 */
bool FioCheckFileExists(const char *filename, Subdirectory subdir)
{
	FILE *f = FioFOpenFile(filename, "rb", subdir);
	if (f == NULL) return false;

	FioFCloseFile(f);
	return true;
}

/**
 * Test whether the given filename exists.
 * @param filename the file to test.
 * @return true if and only if the file exists.
 */
bool FileExists(const char *filename)
{
#if defined(WINCE)
	/* There is always one platform that doesn't support basic commands... */
	HANDLE hand = CreateFile(OTTD2FS(filename), 0, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (hand == INVALID_HANDLE_VALUE) return 1;
	CloseHandle(hand);
	return 0;
#else
	return access(OTTD2FS(filename), 0) == 0;
#endif
}

/**
 * Close a file in a safe way.
 */
void FioFCloseFile(FILE *f)
{
	fclose(f);
}

int FioGetFullPath (char *buf, size_t buflen, Searchpath sp, Subdirectory subdir, const char *filename)
{
	assert(subdir < NUM_SUBDIRS);
	assert(sp < NUM_SEARCHPATHS);

	return snprintf (buf, buflen, "%s%s%s", _searchpaths[sp], _subdirs[subdir], filename != NULL ? filename : "");
}

/**
 * Find a path to the filename in one of the search directories.
 * @param buf [out] Destination buffer for the path.
 * @param buflen Length of the destination buffer.
 * @param subdir Subdirectory to try.
 * @param filename Filename to look for.
 * @return \a buf containing the path if the path was found, else \c NULL.
 */
char *FioFindFullPath(char *buf, size_t buflen, Subdirectory subdir, const char *filename)
{
	Searchpath sp;
	assert(subdir < NUM_SUBDIRS);

	FOR_ALL_SEARCHPATHS(sp) {
		FioGetFullPath(buf, buflen, sp, subdir, filename);
		if (FileExists(buf)) return buf;
#if !defined(WIN32)
		/* Be, as opening files, aware that sometimes the filename
		 * might be in uppercase when it is in lowercase on the
		 * disk. Of course Windows doesn't care about casing. */
		if (strtolower(buf + strlen(_searchpaths[sp]) - 1) && FileExists(buf)) return buf;
#endif
	}

	return NULL;
}

char *FioGetDirectory(char *buf, size_t buflen, Subdirectory subdir)
{
	Searchpath sp;

	/* Find and return the first valid directory */
	FOR_ALL_SEARCHPATHS(sp) {
		FioGetFullPath (buf, buflen, sp, subdir);
		if (FileExists(buf)) return buf;
	}

	/* Could not find the directory, fall back to a base path */
	ttd_strlcpy(buf, _personal_dir, buflen);

	return buf;
}

static FILE *FioFOpenFileSp(const char *filename, const char *mode, Searchpath sp, Subdirectory subdir, size_t *filesize)
{
#if defined(WIN32) && defined(UNICODE)
	/* fopen is implemented as a define with ellipses for
	 * Unicode support (prepend an L). As we are not sending
	 * a string, but a variable, it 'renames' the variable,
	 * so make that variable to makes it compile happily */
	wchar_t Lmode[5];
	MultiByteToWideChar(CP_ACP, 0, mode, -1, Lmode, lengthof(Lmode));
#endif
	FILE *f = NULL;
	char buf[MAX_PATH];

	if (subdir == NO_DIRECTORY) {
		bstrcpy (buf, filename);
	} else {
		bstrfmt (buf, "%s%s%s", _searchpaths[sp], _subdirs[subdir], filename);
	}

#if defined(WIN32)
	if (mode[0] == 'r' && GetFileAttributes(OTTD2FS(buf)) == INVALID_FILE_ATTRIBUTES) return NULL;
#endif

	f = fopen(buf, mode);
#if !defined(WIN32)
	if (f == NULL && strtolower(buf + ((subdir == NO_DIRECTORY) ? 0 : strlen(_searchpaths[sp]) - 1))) {
		f = fopen(buf, mode);
	}
#endif
	if (f != NULL && filesize != NULL) {
		/* Find the size of the file */
		fseek(f, 0, SEEK_END);
		*filesize = ftell(f);
		fseek(f, 0, SEEK_SET);
	}
	return f;
}

/**
 * Opens a file from inside a tar archive.
 * @param entry The entry to open.
 * @param filesize [out] If not \c NULL, size of the opened file.
 * @return File handle of the opened file, or \c NULL if the file is not available.
 * @note The file is read from within the tar file, and may not return \c EOF after reading the whole file.
 */
FILE *FioFOpenFileTar(TarFileListEntry *entry, size_t *filesize)
{
	FILE *f = fopen(entry->tar_filename, "rb");
	if (f == NULL) return f;

	if (fseek(f, entry->position, SEEK_SET) < 0) {
		fclose(f);
		return NULL;
	}

	if (filesize != NULL) *filesize = entry->size;
	return f;
}

/**
 * Opens a OpenTTD file somewhere in a personal or global directory.
 * @param filename Name of the file to open.
 * @param subdir Subdirectory to open.
 * @param filename Name of the file to open.
 * @return File handle of the opened file, or \c NULL if the file is not available.
 */
FILE *FioFOpenFile(const char *filename, const char *mode, Subdirectory subdir, size_t *filesize)
{
	FILE *f = NULL;
	Searchpath sp;

	assert(subdir < NUM_SUBDIRS || subdir == NO_DIRECTORY);

	FOR_ALL_SEARCHPATHS(sp) {
		f = FioFOpenFileSp(filename, mode, sp, subdir, filesize);
		if (f != NULL || subdir == NO_DIRECTORY) break;
	}

	/* We can only use .tar in case of data-dir, and read-mode */
	if (f == NULL && mode[0] == 'r' && subdir != NO_DIRECTORY) {
		static const uint MAX_RESOLVED_LENGTH = 2 * (100 + 100 + 155) + 1; // Enough space to hold two filenames plus link. See 'TarHeader'.
		sstring<MAX_RESOLVED_LENGTH> resolved_name;

		/* Filenames in tars are always forced to be lowercase */
		resolved_name.copy (filename);
		resolved_name.tolower();

		/* Resolve ONE directory link */
		for (TarLinkList::iterator link = TarCache::cache[subdir].links.begin(); link != TarCache::cache[subdir].links.end(); link++) {
			const std::string &src = link->first;
			size_t len = src.length();
			if (resolved_name.length() >= len && resolved_name.c_str()[len - 1] == PATHSEPCHAR && memcmp (src.c_str(), resolved_name.c_str(), len) == 0) {
				/* Apply link */
				char resolved_name2[MAX_RESOLVED_LENGTH];
				bstrcpy (resolved_name2, resolved_name.c_str() + len);
				resolved_name.copy (link->second.c_str());
				resolved_name.append (resolved_name2);
				break; // Only resolve one level
			}
		}

		TarFileList::iterator it = TarCache::cache[subdir].files.find(resolved_name.c_str());
		if (it != TarCache::cache[subdir].files.end()) {
			f = FioFOpenFileTar(&((*it).second), filesize);
		}
	}

	/* Sometimes a full path is given. To support
	 * the 'subdirectory' must be 'removed'. */
	if (f == NULL && subdir != NO_DIRECTORY) {
		switch (subdir) {
			case BASESET_DIR:
				f = FioFOpenFile(filename, mode, OLD_GM_DIR, filesize);
				if (f != NULL) break;
				FALLTHROUGH;
			case NEWGRF_DIR:
				f = FioFOpenFile(filename, mode, OLD_DATA_DIR, filesize);
				break;

			default:
				f = FioFOpenFile(filename, mode, NO_DIRECTORY, filesize);
				break;
		}
	}

	return f;
}

/**
 * Create a directory with the given name
 * @param name the new name of the directory
 */
static void FioCreateDirectory(const char *name)
{
	/* Ignore directory creation errors; they'll surface later on, and most
	 * of the time they are 'directory already exists' errors anyhow. */
#if defined(WIN32) || defined(WINCE)
	CreateDirectory(OTTD2FS(name), NULL);
#elif defined(OS2) && !defined(__INNOTEK_LIBC__)
	mkdir(OTTD2FS(name));
#elif defined(__MORPHOS__) || defined(__AMIGAOS__)
	char buf[MAX_PATH];
	bstrcpy (buf, name);

	size_t len = strlen(name) - 1;
	if (buf[len] == '/') {
		buf[len] = '\0'; // Kill pathsep, so mkdir() will not fail
	}

	mkdir(OTTD2FS(buf), 0755);
#else
	mkdir(OTTD2FS(name), 0755);
#endif
}

/**
 * Appends, if necessary, the path separator character to the end of the string.
 * It does not add the path separator to zero-sized strings.
 * @param buf    string to append the separator to
 * @return true iff the operation succeeded
 */
static bool AppendPathSeparator (stringb *buf)
{
	size_t s = buf->length();

	return (s == 0) || (buf->get_buffer()[s-1] == PATHSEPCHAR)
			|| buf->append (PATHSEPCHAR);
}

/**
 * Construct a path by concatenating the given parts with intervening and
 * trailing path separators, and return it in an newly allocated buffer.
 * @param n the number of components to concatenate
 * @param parts the strings to concatenate
 * @return the resulting path, in a buffer allocated with malloc
 */
char *BuildDirPath (uint n, const char *const *parts)
{
	/* some sanity checks */
	assert (n > 0);
	assert (parts[0][0] != '\0'); // the first string must not be empty

	size_t lengths [4];
	/* change and recompile if you need something bigger */
	assert (n <= lengthof(lengths));

	size_t total = n + 1; // account for path separators and trailing null
	for (uint i = 0; i < n; i++) {
		total += lengths[i] = strlen (parts[i]);
	}

	char *buf = xmalloc (total);
	char *p = buf;

	for (uint i = 0; i < n; i++) {
		assert (p + lengths[i] + 1 < buf + total);
		memcpy (p, parts[i], lengths[i]);
		p += lengths[i];
		/* the first string is not empty, so p > buf */
		if (p[-1] != PATHSEPCHAR) *p++ = PATHSEPCHAR;
	}

	*p = '\0';
	return buf;
}


void TarCache::add_link (const std::string &srcp, const std::string &destp)
{
	std::string src = srcp;
	std::string dest = destp;
	/* Tar internals assume lowercase */
	std::transform(src.begin(), src.end(), src.begin(), tolower);
	std::transform(dest.begin(), dest.end(), dest.begin(), tolower);

	TarFileList::iterator dest_file = this->files.find(dest);
	if (dest_file != this->files.end()) {
		/* Link to file. Process the link like the destination file. */
		this->files.insert (TarFileList::value_type (src, dest_file->second));
	} else {
		/* Destination file not found. Assume 'link to directory'
		 * Append PATHSEPCHAR to 'src' and 'dest' if needed */
		const std::string src_path = ((*src.rbegin() == PATHSEPCHAR) ? src : src + PATHSEPCHAR);
		const std::string dst_path = (dest.length() == 0 ? "" : ((*dest.rbegin() == PATHSEPCHAR) ? dest : dest + PATHSEPCHAR));
		this->links.insert (TarLinkList::value_type (src_path, dst_path));
	}
}

/**
 * Simplify filenames from tars.
 * Replace '/' by #PATHSEPCHAR, and force 'name' to lowercase.
 * @param name Filename to process.
 */
static void SimplifyFileName(char *name)
{
	/* Force lowercase */
	strtolower(name);

	/* Tar-files always have '/' path-separator, but we want our PATHSEPCHAR */
#if (PATHSEPCHAR != '/')
	for (char *n = name; *n != '\0'; n++) if (*n == '/') *n = PATHSEPCHAR;
#endif
}

/**
 * Perform the scanning of a particular subdirectory.
 * @param subdir The subdirectory to scan.
 * @return The number of found tar files.
 */
uint TarScanner::DoScan(Subdirectory sd)
{
	TarCache::cache[sd].files.clear();
	TarCache::cache[sd].tars.clear();
	uint num = this->Scan (sd);
	if (sd == BASESET_DIR || sd == NEWGRF_DIR) num += this->Scan (OLD_DATA_DIR);
	return num;
}

/* static */ uint TarScanner::DoScan(TarScanner::Mode mode)
{
	DEBUG(misc, 1, "Scanning for tars");
	TarScanner fs;
	uint num = 0;
	if (mode & TarScanner::BASESET) {
		num += fs.DoScan(BASESET_DIR);
	}
	if (mode & TarScanner::NEWGRF) {
		num += fs.DoScan(NEWGRF_DIR);
	}
	if (mode & TarScanner::AI) {
		num += fs.DoScan(AI_DIR);
		num += fs.DoScan(AI_LIBRARY_DIR);
	}
	if (mode & TarScanner::GAME) {
		num += fs.DoScan(GAME_DIR);
		num += fs.DoScan(GAME_LIBRARY_DIR);
	}
	if (mode & TarScanner::SCENARIO) {
		num += fs.DoScan(SCENARIO_DIR);
		num += fs.DoScan(HEIGHTMAP_DIR);
	}
	DEBUG(misc, 1, "Scan complete, found %d files", num);
	return num;
}

/**
 * Add a scanned file to the scanned files of a tar.
 * @param filename        the full path to the file to read
 * @param basepath_length amount of characters to chop of before to get a
 *                        filename relative to the search path.
 * @param tar_filename    the name of the tar file the file is read from.
 * @return true if the file is added.
 */
bool TarScanner::AddFile(const char *filename, size_t basepath_length, const char *tar_filename)
{
	/* No tar within tar. */
	assert(tar_filename == NULL);

	return TarCache::cache[this->subdir].add (filename, basepath_length);
}

/**
 * Add a scanned file to a tar cache.
 * @param filename        the full path to the file to read
 * @param basepath_length amount of characters to chop of before to get a
 *                        filename relative to the search path.
 * @return true if the file is added.
 */
bool TarCache::add (const char *filename, size_t basepath_length)
{
	/* The TAR-header, repeated for every file */
	struct TarHeader {
		char name[100];      ///< Name of the file
		char mode[8];
		char uid[8];
		char gid[8];
		char size[12];       ///< Size of the file, in ASCII
		char mtime[12];
		char chksum[8];
		char typeflag;
		char linkname[100];
		char magic[6];
		char version[2];
		char uname[32];
		char gname[32];
		char devmajor[8];
		char devminor[8];
		char prefix[155];    ///< Path of the file

		char unused[12];
	};

	assert_compile (sizeof(TarHeader) == 512);

	/* Check if we already seen this file */
	TarList::iterator it = this->tars.find(filename);
	if (it != this->tars.end()) return false;

	FILE *f = fopen(filename, "rb");
	/* Although the file has been found there can be
	 * a number of reasons we cannot open the file.
	 * Most common case is when we simply have not
	 * been given read access. */
	if (f == NULL) return false;

	char *dupped_filename = xstrdup(filename);
	this->tars[filename].filename.reset (dupped_filename);
	this->tars[filename].dirname.reset();

	TarLinkList links; ///< Temporary list to collect links
	size_t num = 0, pos = 0;

	for (;;) { // Note: feof() always returns 'false' after 'fseek()'. Cool, isn't it?
		TarHeader th;
		if (fread (&th, 1, 512, f) != 512) break;
		pos += 512;

		/* Check if we have the new tar-format (ustar). */
		if (strncmp(th.magic, "ustar", 5) != 0) {
			/* Check if we have the old one (header zeroed after 'link' field). */
			if ((th.magic[0] != '\0') || (memcmp (&th.magic[0], &th.magic[1], 511 - offsetof(TarHeader, magic)) != 0)) {
				DEBUG(misc, 0, "The file '%s' isn't a valid tar-file", filename);
				fclose(f);
				return false;
			}
		}

		/* The prefix contains the directory-name */
		char name[sizeof(th.prefix) + 1 + sizeof(th.name) + 1];
		if (th.prefix[0] != '\0') {
			bstrfmt (name, "%.*s" PATHSEP "%.*s",
				(int)sizeof(th.prefix), th.prefix,
				(int)sizeof(th.name), th.name);
		} else {
			bstrfmt (name, "%.*s", (int)sizeof(th.name), th.name);
		}

		/* Calculate the size of the file.. for some strange reason this is stored as a string */
		char buf[sizeof(th.size) + 1];
		bstrcpy (buf, th.size);
		size_t skip = strtoul(buf, NULL, 8);

		switch (th.typeflag) {
			case '\0':
			case '0': { // regular file
				/* Ignore empty files */
				if (skip == 0) break;

				if (strlen(name) == 0) break;

				/* Store this entry in the list */
				TarFileListEntry entry;
				entry.tar_filename = dupped_filename;
				entry.size         = skip;
				entry.position     = pos;

				/* Convert to lowercase and our PATHSEPCHAR */
				SimplifyFileName(name);

				DEBUG(misc, 6, "Found file in tar: %s (" PRINTF_SIZE " bytes, " PRINTF_SIZE " offset)", name, skip, pos);
				if (this->files.insert(TarFileList::value_type(name, entry)).second) num++;

				break;
			}

			case '1': // hard links
			case '2': { // symbolic links
				/* Copy the destination of the link in a safe way at the end of 'linkname' */
				char link[sizeof(th.linkname) + 1];
				bstrcpy (link, th.linkname);

				if (strlen(name) == 0 || strlen(link) == 0) break;

				/* Convert to lowercase and our PATHSEPCHAR */
				SimplifyFileName(name);
				SimplifyFileName(link);

				/* Only allow relative links */
				if (link[0] == PATHSEPCHAR) {
					DEBUG(misc, 1, "Ignoring absolute link in tar: %s -> %s", name, link);
					break;
				}

				/* Process relative path.
				 * Note: The destination of links must not contain any directory-links. */
				char dest[sizeof(name) + 1 + sizeof(th.linkname) + 1];
				bstrcpy (dest, name);
				char *destpos = strrchr(dest, PATHSEPCHAR);
				if (destpos == NULL) destpos = dest;
				*destpos = '\0';

				const char *pos = link;
				for (;;) {
					const char *next = strchr (pos, PATHSEPCHAR);
					if (next == NULL) next = pos + strlen(pos);

					/* Skip '.' (current dir) */
					if (next != pos + 1 || pos[0] != '.') {
						if (next == pos + 2 && pos[0] == '.' && pos[1] == '.') {
							/* level up */
							if (dest[0] == '\0') {
								DEBUG(misc, 1, "Ignoring link pointing outside of data directory: %s -> %s", name, link);
								break;
							}

							/* Truncate 'dest' after last PATHSEPCHAR.
							 * This assumes that the truncated part is a real directory and not a link. */
							destpos = strrchr(dest, PATHSEPCHAR);
							if (destpos == NULL) destpos = dest;
						} else {
							/* Append at end of 'dest' */
							if (destpos != dest) *(destpos++) = PATHSEPCHAR;
							uint len = next - pos;
							assert (destpos + len < endof(dest));
							memcpy (destpos, pos, len); // Safe as we do '\0'-termination ourselves
							destpos += next - pos;
						}
						*destpos = '\0';
					}

					if (*next == '\0') break;

					pos = next + 1;
				}

				/* Store links in temporary list */
				DEBUG(misc, 6, "Found link in tar: %s -> %s", name, dest);
				links.insert(TarLinkList::value_type(name, dest));

				break;
			}

			case '5': // directory
				/* Convert to lowercase and our PATHSEPCHAR */
				SimplifyFileName(name);

				/* Store the first directory name we detect */
				DEBUG(misc, 6, "Found dir in tar: %s", name);
				if (!this->tars[filename].dirname) {
					this->tars[filename].dirname.reset (xstrdup(name));
				}
				break;

			default:
				/* Ignore other types */
				break;
		}

		/* Skip to the next block.. */
		skip = Align(skip, 512);
		if (fseek(f, skip, SEEK_CUR) < 0) {
			DEBUG(misc, 0, "The file '%s' can't be read as a valid tar-file", filename);
			fclose(f);
			return false;
		}
		pos += skip;
	}

	DEBUG(misc, 1, "Found tar '%s' with " PRINTF_SIZE " new files", filename, num);
	fclose(f);

	/* Resolve file links and store directory links.
	 * We restrict usage of links to two cases:
	 *  1) Links to directories:
	 *      Both the source path and the destination path must NOT contain any further links.
	 *      When resolving files at most one directory link is resolved.
	 *  2) Links to files:
	 *      The destination path must NOT contain any links.
	 *      The source path may contain one directory link.
	 */
	for (TarLinkList::iterator link = links.begin(); link != links.end(); link++) {
		this->add_link (link->first, link->second);
	}

	return true;
}

/**
 * Extract the tar with the given filename in the directory
 * where the tar resides.
 * @param tar_filename the name of the tar to extract.
 * @return false on failure.
 */
bool TarCache::extract (const char *tar_filename)
{
	TarList::iterator it = this->tars.find(tar_filename);
	/* We don't know the file. */
	if (it == this->tars.end()) return false;

	/* The file doesn't have a sub directory! */
	if (!(*it).second.dirname) return false;

	const char *p = strrchr (tar_filename, PATHSEPCHAR);
	/* The file's path does not have a separator? */
	if (p == NULL) return false;
	size_t base_length = p - tar_filename + 1;

	sstring<MAX_PATH> filename;
	filename.fmt ("%.*s%s", (int)base_length, tar_filename, (*it).second.dirname.get());
	DEBUG (misc, 8, "Extracting %s to directory %s", tar_filename, filename.c_str());
	FioCreateDirectory (filename.c_str());

	for (TarFileList::iterator it2 = this->files.begin(); it2 != this->files.end(); it2++) {
		if (strcmp((*it2).second.tar_filename, tar_filename) != 0) continue;

		filename.truncate (base_length);
		filename.append ((*it2).first.c_str());

		DEBUG(misc, 9, "  extracting %s", filename.c_str());

		/* First open the file in the .tar. */
		size_t to_copy = 0;
		FILE *in = FioFOpenFileTar(&(*it2).second, &to_copy);
		if (in == NULL) {
			DEBUG(misc, 6, "Extracting %s failed; could not open %s", filename.c_str(), tar_filename);
			return false;
		}

		/* Now open the 'output' file. */
		FILE *out = fopen (filename.c_str(), "wb");
		if (out == NULL) {
			DEBUG(misc, 6, "Extracting %s failed; could not open %s", filename.c_str(), filename.c_str());
			fclose(in);
			return false;
		}

		/* Now read from the tar and write it into the file. */
		char buffer[4096];
		size_t read;
		for (; to_copy != 0; to_copy -= read) {
			read = fread(buffer, 1, min(to_copy, lengthof(buffer)), in);
			if (read <= 0 || fwrite(buffer, 1, read, out) != read) break;
		}

		/* Close everything up. */
		fclose(in);
		fclose(out);

		if (to_copy != 0) {
			DEBUG(misc, 6, "Extracting %s failed; still %i bytes to copy", filename.c_str(), (int)to_copy);
			return false;
		}
	}

	DEBUG(misc, 9, "  extraction successful");
	return true;
}

#if defined(WIN32) || defined(WINCE)
/**
 * Determine the base (personal dir and game data dir) paths
 * @param exe the path from the current path to the executable
 * @note defined in the OS related files (os2.cpp, win32.cpp, unix.cpp etc)
 */
extern void DetermineBasePaths(const char *exe);
#else /* defined(WIN32) || defined(WINCE) */

/**
 * Changes the working directory to the path of the give executable.
 * For OSX application bundles '.app' is the required extension of the bundle,
 * so when we crop the path to there, when can remove the name of the bundle
 * in the same way we remove the name from the executable name.
 * @param exe the path to the executable
 */
static bool ChangeWorkingDirectoryToExecutable(const char *exe)
{
	char tmp[MAX_PATH];
	bstrcpy (tmp, exe);

	bool success = false;
#ifdef WITH_COCOA
	char *app_bundle = strchr(tmp, '.');
	while (app_bundle != NULL && strncasecmp(app_bundle, ".app", 4) != 0) app_bundle = strchr(&app_bundle[1], '.');

	if (app_bundle != NULL) *app_bundle = '\0';
#endif /* WITH_COCOA */
	char *s = strrchr(tmp, PATHSEPCHAR);
	if (s != NULL) {
		*s = '\0';
#if defined(__DJGPP__)
		/* If we want to go to the root, we can't use cd C:, but we must use '/' */
		if (s > tmp && *(s - 1) == ':') chdir("/");
#endif
		if (chdir(tmp) != 0) {
			DEBUG(misc, 0, "Directory with the binary does not exist?");
		} else {
			success = true;
		}
	}
	return success;
}

/**
 * Whether we should scan the working directory.
 * It should not be scanned if it's the root or
 * the home directory as in both cases a big data
 * directory can cause huge amounts of unrelated
 * files scanned. Furthermore there are nearly no
 * use cases for the home/root directory to have
 * OpenTTD directories.
 * @return true if it should be scanned.
 */
static bool DoScanWorkingDirectory()
{
	/* No working directory, so nothing to do. */
	if (_searchpaths[SP_WORKING_DIR] == NULL) return false;

	/* Working directory is root, so do nothing. */
	if (strcmp(_searchpaths[SP_WORKING_DIR], PATHSEP) == 0) return false;

	/* No personal/home directory, so the working directory won't be that. */
	if (_searchpaths[SP_PERSONAL_DIR] == NULL) return true;

	sstring<MAX_PATH> tmp;
	tmp.fmt ("%s%s", _searchpaths[SP_WORKING_DIR], PERSONAL_DIR);
	AppendPathSeparator (&tmp);
	return strcmp (tmp.c_str(), _searchpaths[SP_PERSONAL_DIR]) != 0;
}

/** strdup the current working directory, with a trailing path separator. */
static char *dupcwd (void)
{
	char tmp [MAX_PATH];
	if (getcwd (tmp, MAX_PATH) == NULL) *tmp = '\0';
	return BuildDirPath (tmp);
}

/**
 * Determine the base (personal dir and game data dir) paths
 * @param exe the path to the executable
 */
static void DetermineBasePaths(const char *exe)
{
#if defined(WITH_XDG_BASEDIR) && defined(WITH_PERSONAL_DIR)
	const char *xdg_data_home = xdgDataHome(NULL);
	_searchpaths[SP_PERSONAL_DIR_XDG] = BuildDirPath (xdg_data_home,
			PERSONAL_DIR[0] == '.' ? &PERSONAL_DIR[1] : PERSONAL_DIR);
	free(xdg_data_home);
#endif
#if defined(__MORPHOS__) || defined(__AMIGA__) || defined(DOS) || defined(OS2) || !defined(WITH_PERSONAL_DIR)
	_searchpaths[SP_PERSONAL_DIR] = NULL;
#else
#ifdef __HAIKU__
	BPath path;
	find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	char *homedir = xstrdup(path.Path());
#else
	/* getenv is highly unsafe; duplicate it as soon as possible,
	 * or at least before something else touches the environment
	 * variables in any way. It can also contain all kinds of
	 * unvalidated data we rather not want internally. */
	char *homedir = getenv("HOME");
	if (homedir != NULL) {
		homedir = xstrdup (homedir);
	} else {
		const struct passwd *pw = getpwuid(getuid());
		homedir = (pw == NULL) ? NULL : xstrdup(pw->pw_dir);
	}
#endif

	if (homedir != NULL) {
		ValidateString(homedir);
		_searchpaths[SP_PERSONAL_DIR] = BuildDirPath (homedir, PERSONAL_DIR);
		free(homedir);
	} else {
		_searchpaths[SP_PERSONAL_DIR] = NULL;
	}
#endif

#if defined(WITH_SHARED_DIR)
	_searchpaths[SP_SHARED_DIR] = BuildDirPath (SHARED_DIR);
#else
	_searchpaths[SP_SHARED_DIR] = NULL;
#endif

#if defined(__MORPHOS__) || defined(__AMIGA__)
	_searchpaths[SP_WORKING_DIR] = NULL;
#else
	_searchpaths[SP_WORKING_DIR] = dupcwd();
#endif

	_do_scan_working_directory = DoScanWorkingDirectory();

	/* Change the working directory to that one of the executable */
	if (ChangeWorkingDirectoryToExecutable(exe)) {
		_searchpaths[SP_BINARY_DIR] = dupcwd();
	} else {
		_searchpaths[SP_BINARY_DIR] = NULL;
	}

	if (_searchpaths[SP_WORKING_DIR] != NULL) {
		/* Go back to the current working directory. */
		if (chdir(_searchpaths[SP_WORKING_DIR]) != 0) {
			DEBUG(misc, 0, "Failed to return to working directory!");
		}
	}

#if defined(__MORPHOS__) || defined(__AMIGA__) || defined(DOS) || defined(OS2)
	_searchpaths[SP_INSTALLATION_DIR] = NULL;
#else
	_searchpaths[SP_INSTALLATION_DIR] = BuildDirPath (GLOBAL_DATA_DIR);
#endif
#ifdef WITH_COCOA
extern char *cocoaSetApplicationBundleDir();
	_searchpaths[SP_APPLICATION_BUNDLE_DIR] = cocoaSetApplicationBundleDir();
#else
	_searchpaths[SP_APPLICATION_BUNDLE_DIR] = NULL;
#endif
}
#endif /* defined(WIN32) || defined(WINCE) */

const char *_personal_dir;

/**
 * Acquire the base paths (personal dir and game data dir),
 * fill all other paths (save dir, autosave dir etc) and
 * make the save and scenario directories.
 * @param exe the path from the current path to the executable
 */
void DeterminePaths(const char *exe)
{
	DetermineBasePaths(exe);

	Searchpath sp;
	FOR_ALL_SEARCHPATHS(sp) {
		if (sp == SP_WORKING_DIR && !_do_scan_working_directory) continue;
		DEBUG(misc, 4, "%s added as search path", _searchpaths[sp]);
	}

	char config_buffer[MAX_PATH];
	const char *config_dir;
	if (_config_file != NULL) {
		char *end = strrchr(_config_file, PATHSEPCHAR);
		config_dir = (end == NULL) ? "" : xstrmemdup (_config_file, end - _config_file);
	} else {
		if (FioFindFullPath (config_buffer, lengthof(config_buffer), BASE_DIR, "openttd.cfg") != NULL) {
			char *end = strrchr (config_buffer, PATHSEPCHAR);
			if (end != NULL) end[1] = '\0';
			config_dir = xstrdup (config_buffer);
		} else {
#if defined(WITH_XDG_BASEDIR) && defined(WITH_PERSONAL_DIR)
			/* No previous configuration file found. Use the configuration folder from XDG. */
			stringb config_home (config_buffer);
			const char *xdg_config_home = xdgConfigHome (NULL);
			config_home.fmt ("%s" PATHSEP "%s", xdg_config_home,
					PERSONAL_DIR[0] == '.' ? &PERSONAL_DIR[1] : PERSONAL_DIR);
			free (xdg_config_home);

			AppendPathSeparator (&config_home);

			config_dir = config_buffer;
#else
			static const Searchpath new_openttd_cfg_order[] = {
					SP_PERSONAL_DIR, SP_BINARY_DIR, SP_WORKING_DIR, SP_SHARED_DIR, SP_INSTALLATION_DIR
				};

			for (uint i = 0; ; i++) {
				assert (i < lengthof(new_openttd_cfg_order));
				config_dir = _searchpaths[new_openttd_cfg_order[i]];
				if (config_dir != NULL) break;
			}
#endif
		}
		_config_file = str_fmt ("%sopenttd.cfg", config_dir);
	}

	DEBUG(misc, 3, "%s found as config directory", config_dir);

	_highscore_file = str_fmt("%shs.dat", config_dir);
	extern char *_hotkeys_file;
	_hotkeys_file = str_fmt("%shotkeys.cfg", config_dir);
	extern char *_windows_file;
	_windows_file = str_fmt("%swindows.cfg", config_dir);

#if defined(WITH_XDG_BASEDIR) && defined(WITH_PERSONAL_DIR)
	if (config_dir == config_buffer) {
		/* We are using the XDG configuration home for the config file,
		 * then store the rest in the XDG data home folder. */
		_personal_dir = _searchpaths[SP_PERSONAL_DIR_XDG];
		FioCreateDirectory(_personal_dir);
	} else
#endif
	{
		_personal_dir = config_dir;
	}

	/* Make the necessary folders */
#if !defined(__MORPHOS__) && !defined(__AMIGA__) && defined(WITH_PERSONAL_DIR)
	FioCreateDirectory(config_dir);
#endif

	DEBUG(misc, 3, "%s found as personal directory", _personal_dir);

	static const Subdirectory default_subdirs[] = {
		SAVE_DIR, AUTOSAVE_DIR, SCENARIO_DIR, HEIGHTMAP_DIR, BASESET_DIR, NEWGRF_DIR, AI_DIR, AI_LIBRARY_DIR, GAME_DIR, GAME_LIBRARY_DIR, SCREENSHOT_DIR
	};

	for (uint i = 0; i < lengthof(default_subdirs); i++) {
		char *dir = str_fmt("%s%s", _personal_dir, _subdirs[default_subdirs[i]]);
		FioCreateDirectory(dir);
		free(dir);
	}

	/* If we have network we make a directory for the autodownloading of content */
	_searchpaths[SP_AUTODOWNLOAD_DIR] = str_fmt("%s%s", _personal_dir, "content_download" PATHSEP);
#ifdef ENABLE_NETWORK
	FioCreateDirectory(_searchpaths[SP_AUTODOWNLOAD_DIR]);

	/* Create the directory for each of the types of content */
	const Subdirectory dirs[] = { SCENARIO_DIR, HEIGHTMAP_DIR, BASESET_DIR, NEWGRF_DIR, AI_DIR, AI_LIBRARY_DIR, GAME_DIR, GAME_LIBRARY_DIR };
	for (uint i = 0; i < lengthof(dirs); i++) {
		char *tmp = str_fmt("%s%s", _searchpaths[SP_AUTODOWNLOAD_DIR], _subdirs[dirs[i]]);
		FioCreateDirectory(tmp);
		free(tmp);
	}

	extern char *_log_file;
	_log_file = str_fmt("%sopenttd.log",  _personal_dir);
#else /* ENABLE_NETWORK */
	/* If we don't have networking, we don't need to make the directory. But
	 * if it exists we keep it, otherwise remove it from the search paths. */
	if (!FileExists(_searchpaths[SP_AUTODOWNLOAD_DIR]))  {
		free(_searchpaths[SP_AUTODOWNLOAD_DIR]);
		_searchpaths[SP_AUTODOWNLOAD_DIR] = NULL;
	}
#endif /* ENABLE_NETWORK */
}

/**
 * Sanitizes a filename, i.e. removes all illegal characters from it.
 * @param filename the "\0" terminated filename
 */
void SanitizeFilename(char *filename)
{
	for (; *filename != '\0'; filename++) {
		switch (*filename) {
			/* The following characters are not allowed in filenames
			 * on at least one of the supported operating systems: */
			case ':': case '\\': case '*': case '?': case '/':
			case '<': case '>': case '|': case '"':
				*filename = '_';
				break;
		}
	}
}

/**
 * Load a file into memory.
 * @param filename Name of the file to load.
 * @param lenp [out] Length of loaded data.
 * @param maxsize Maximum size to load.
 * @return Pointer to new memory containing the loaded data, or \c NULL if loading failed.
 * @note If \a maxsize less than the length of the file, loading fails.
 */
void *ReadFileToMem(const char *filename, size_t *lenp, size_t maxsize)
{
	FILE *in = fopen(filename, "rb");
	if (in == NULL) return NULL;

	fseek(in, 0, SEEK_END);
	size_t len = ftell(in);
	fseek(in, 0, SEEK_SET);
	if (len > maxsize) {
		fclose(in);
		return NULL;
	}
	byte *mem = xmalloct<byte>(len + 1);
	mem[len] = 0;
	if (fread(mem, len, 1, in) != 1) {
		fclose(in);
		free(mem);
		return NULL;
	}
	fclose(in);

	*lenp = len;
	return mem;
}

/**
 * Helper to see whether a given filename matches the extension.
 * @param extension The extension to look for.
 * @param filename  The filename to look in for the extension.
 * @return True iff the extension is NULL, or the filename ends with it.
 */
static bool MatchesExtension(const char *extension, const char *filename)
{
	if (extension == NULL) return true;

	const char *ext = strrchr(filename, extension[0]);
	return ext != NULL && strcasecmp(ext, extension) == 0;
}

/**
 * Scan a single directory (and recursively its children) and add
 * any graphics sets that are found.
 * @param fs              the file scanner to add the files to
 * @param extension       the extension of files to search for.
 * @param path            full path we're currently at
 * @param basepath_length from where in the path are we 'based' on the search path
 * @param recursive       whether to recursively search the sub directories
 */
static uint ScanPath(FileScanner *fs, const char *extension, const char *path, size_t basepath_length, bool recursive)
{
	extern bool FiosIsValidFile(const char *path, const struct dirent *ent, struct stat *sb);

	uint num = 0;
	struct stat sb;
	struct dirent *dirent;
	DIR *dir;

	if (path == NULL || (dir = ttd_opendir(path)) == NULL) return 0;

	while ((dirent = readdir(dir)) != NULL) {
		const char *d_name = FS2OTTD(dirent->d_name);
		sstring<MAX_PATH> filename;

		if (!FiosIsValidFile(path, dirent, &sb)) continue;

		filename.fmt ("%s%s", path, d_name);

		if (S_ISDIR(sb.st_mode)) {
			/* Directory */
			if (!recursive) continue;
			if (strcmp(d_name, ".") == 0 || strcmp(d_name, "..") == 0) continue;
			if (!AppendPathSeparator (&filename)) continue;
			num += ScanPath (fs, extension, filename.c_str(), basepath_length, recursive);
		} else if (S_ISREG(sb.st_mode)) {
			/* File */
			if (MatchesExtension (extension, filename.c_str()) && fs->AddFile (filename.c_str(), basepath_length, NULL)) num++;
		}
	}

	closedir(dir);

	return num;
}

/**
 * Scan the given tar and add graphics sets when it finds one.
 * @param fs        the file scanner to scan for
 * @param extension the extension of files to search for.
 * @param tar       the tar to search in.
 */
static uint ScanTar(FileScanner *fs, const char *extension, TarFileList::iterator tar)
{
	uint num = 0;
	const char *filename = (*tar).first.c_str();

	if (MatchesExtension(extension, filename) && fs->AddFile(filename, 0, (*tar).second.tar_filename)) num++;

	return num;
}

/**
 * Scan for files with the given extension in the given search path.
 * @param extension the extension of files to search for.
 * @param sd        the sub directory to search in.
 * @param tars      whether to search in the tars too.
 * @param recursive whether to search recursively
 * @return the number of found files, i.e. the number of times that
 *         AddFile returned true.
 */
uint FileScanner::Scan(const char *extension, Subdirectory sd, bool tars, bool recursive)
{
	this->subdir = sd;

	Searchpath sp;
	char path[MAX_PATH];
	TarFileList::iterator tar;
	uint num = 0;

	FOR_ALL_SEARCHPATHS(sp) {
		/* Don't search in the working directory */
		if (sp == SP_WORKING_DIR && !_do_scan_working_directory) continue;

		FioGetFullPath (path, MAX_PATH, sp, sd);
		num += ScanPath(this, extension, path, strlen(path), recursive);
	}

	if (tars && sd != NO_DIRECTORY) {
		FOR_ALL_TARS(tar, sd) {
			num += ScanTar(this, extension, tar);
		}
	}

	switch (sd) {
		case BASESET_DIR:
			num += this->Scan(extension, OLD_GM_DIR, tars, recursive);
			FALLTHROUGH;
		case NEWGRF_DIR:
			num += this->Scan(extension, OLD_DATA_DIR, tars, recursive);
			break;

		default: break;
	}

	return num;
}

/**
 * Scan for files with the given extension in the given search path.
 * @param extension the extension of files to search for.
 * @param directory the sub directory to search in.
 * @param dirend if not null, end directory here
 * @param recursive whether to search recursively
 * @return the number of found files, i.e. the number of times that
 *         AddFile returned true.
 */
uint FileScanner::Scan (const char *extension, const char *directory,
	const char *dirend, bool recursive)
{
	sstring<MAX_PATH> path;
	if (dirend == NULL) {
		path.copy (directory);
	} else {
		path.fmt ("%.*s", (int)(dirend - directory), directory);
	}
	if (!AppendPathSeparator (&path)) return 0;
	return ScanPath (this, extension, path.c_str(), path.length(), recursive);
}
