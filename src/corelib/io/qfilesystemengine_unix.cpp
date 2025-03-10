/****************************************************************************
**
** Copyright (C) 2018 Intel Corporation.
** Copyright (C) 2016 The Qt Company Ltd.
** Copyright (C) 2013 Samuel Gaist <samuel.gaist@edeltech.ch>
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtCore module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qplatformdefs.h"
#include "qfilesystemengine_p.h"
#include "qfile.h"
#include "qstorageinfo.h"

#include <QtCore/qoperatingsystemversion.h>
#include <QtCore/private/qcore_unix_p.h>
#include <QtCore/private/qfiledevice_p.h>
#include <QtCore/qvarlengtharray.h>
#ifndef QT_BOOTSTRAPPED
# include <QtCore/qstandardpaths.h>
#endif // QT_BOOTSTRAPPED

#include <pwd.h>
#include <stdlib.h> // for realpath()
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

#if __has_include(<paths.h>)
# include <paths.h>
#endif
#ifndef _PATH_TMP           // from <paths.h>
# define _PATH_TMP          "/tmp"
#endif

#if defined(Q_OS_MAC)
# include <QtCore/private/qcore_mac_p.h>
# include <CoreFoundation/CFBundle.h>
#endif

#ifdef Q_OS_MACOS
#include <CoreServices/CoreServices.h>
#endif

#if defined(QT_PLATFORM_UIKIT)
#include <MobileCoreServices/MobileCoreServices.h>
#endif

#if defined(Q_OS_DARWIN)
# include <sys/clonefile.h>
# include <copyfile.h>
// We cannot include <Foundation/Foundation.h> (it's an Objective-C header), but
// we need these declarations:
Q_FORWARD_DECLARE_OBJC_CLASS(NSString);
extern "C" NSString *NSTemporaryDirectory();
#endif

#if defined(Q_OS_LINUX)
#  include <sys/ioctl.h>
#  include <sys/sendfile.h>
#  include <linux/fs.h>

// in case linux/fs.h is too old and doesn't define it:
#ifndef FICLONE
#  define FICLONE       _IOW(0x94, 9, int)
#endif
#endif

#if defined(Q_OS_ANDROID)
// statx() is disabled on Android because quite a few systems
// come with sandboxes that kill applications that make system calls outside a
// whitelist and several Android vendors can't be bothered to update the list.
#  undef STATX_BASIC_STATS
#endif

#ifndef STATX_ALL
struct statx { mode_t stx_mode; };      // dummy
#endif

QT_BEGIN_NAMESPACE

enum {
#ifdef Q_OS_ANDROID
    // On Android, the link(2) system call has been observed to always fail
    // with EACCES, regardless of whether there are permission problems or not.
    SupportsHardlinking = false
#else
    SupportsHardlinking = true
#endif
};

#if defined(Q_OS_DARWIN)
static inline bool hasResourcePropertyFlag(const QFileSystemMetaData &data,
                                           const QFileSystemEntry &entry,
                                           CFStringRef key)
{
    QCFString path = CFStringCreateWithFileSystemRepresentation(0,
        entry.nativeFilePath().constData());
    if (!path)
        return false;

    QCFType<CFURLRef> url = CFURLCreateWithFileSystemPath(0, path, kCFURLPOSIXPathStyle,
        data.hasFlags(QFileSystemMetaData::DirectoryType));
    if (!url)
        return false;

    CFBooleanRef value;
    if (CFURLCopyResourcePropertyForKey(url, key, &value, NULL)) {
        if (value == kCFBooleanTrue)
            return true;
    }

    return false;
}

static bool isPackage(const QFileSystemMetaData &data, const QFileSystemEntry &entry)
{
    if (!data.isDirectory())
        return false;

    QFileInfo info(entry.filePath());
    QString suffix = info.suffix();

    if (suffix.length() > 0) {
        // First step: is the extension known ?
        QCFType<CFStringRef> extensionRef = suffix.toCFString();
        QCFType<CFStringRef> uniformTypeIdentifier = UTTypeCreatePreferredIdentifierForTag(kUTTagClassFilenameExtension, extensionRef, NULL);
        if (UTTypeConformsTo(uniformTypeIdentifier, kUTTypeBundle))
            return true;

        // Second step: check if an application knows the package type
        QCFType<CFStringRef> path = entry.filePath().toCFString();
        QCFType<CFURLRef> url = CFURLCreateWithFileSystemPath(0, path, kCFURLPOSIXPathStyle, true);

        UInt32 type, creator;
        // Well created packages have the PkgInfo file
        if (CFBundleGetPackageInfoInDirectory(url, &type, &creator))
            return true;

#ifdef Q_OS_MACOS
        // Find if an application other than Finder claims to know how to handle the package
        QCFType<CFURLRef> application = LSCopyDefaultApplicationURLForURL(url,
            kLSRolesEditor | kLSRolesViewer, nullptr);

        if (application) {
            QCFType<CFBundleRef> bundle = CFBundleCreate(kCFAllocatorDefault, application);
            CFStringRef identifier = CFBundleGetIdentifier(bundle);
            QString applicationId = QString::fromCFString(identifier);
            if (applicationId != QLatin1String("com.apple.finder"))
                return true;
        }
#endif
    }

    // Third step: check if the directory has the package bit set
    return hasResourcePropertyFlag(data, entry, kCFURLIsPackageKey);
}
#endif

namespace {
namespace GetFileTimes {
#if !QT_CONFIG(futimens) && (QT_CONFIG(futimes))
template <typename T>
static inline typename std::enable_if_t<(&T::st_atim, &T::st_mtim, true)> get(const T *p, struct timeval *access, struct timeval *modification)
{
    access->tv_sec = p->st_atim.tv_sec;
    access->tv_usec = p->st_atim.tv_nsec / 1000;

    modification->tv_sec = p->st_mtim.tv_sec;
    modification->tv_usec = p->st_mtim.tv_nsec / 1000;
}

template <typename T>
static inline typename std::enable_if_t<(&T::st_atimespec, &T::st_mtimespec, true)> get(const T *p, struct timeval *access, struct timeval *modification)
{
    access->tv_sec = p->st_atimespec.tv_sec;
    access->tv_usec = p->st_atimespec.tv_nsec / 1000;

    modification->tv_sec = p->st_mtimespec.tv_sec;
    modification->tv_usec = p->st_mtimespec.tv_nsec / 1000;
}

#  ifndef st_atimensec
// if "st_atimensec" is defined, this would expand to invalid C++
template <typename T>
static inline typename std::enable_if_t<(&T::st_atimensec, &T::st_mtimensec, true)> get(const T *p, struct timeval *access, struct timeval *modification)
{
    access->tv_sec = p->st_atime;
    access->tv_usec = p->st_atimensec / 1000;

    modification->tv_sec = p->st_mtime;
    modification->tv_usec = p->st_mtimensec / 1000;
}
#  endif
#endif

qint64 timespecToMSecs(const timespec &spec)
{
    return (qint64(spec.tv_sec) * 1000) + (spec.tv_nsec / 1000000);
}

// fallback set
[[maybe_unused]] qint64 atime(const QT_STATBUF &statBuffer, ulong) { return qint64(statBuffer.st_atime) * 1000; }
[[maybe_unused]] qint64 birthtime(const QT_STATBUF &, ulong)       { return Q_INT64_C(0); }
[[maybe_unused]] qint64 ctime(const QT_STATBUF &statBuffer, ulong) { return qint64(statBuffer.st_ctime) * 1000; }
[[maybe_unused]] qint64 mtime(const QT_STATBUF &statBuffer, ulong) { return qint64(statBuffer.st_mtime) * 1000; }

// Xtim, POSIX.1-2008
template <typename T>
[[maybe_unused]] static typename std::enable_if<(&T::st_atim, true), qint64>::type
atime(const T &statBuffer, int)
{ return timespecToMSecs(statBuffer.st_atim); }

template <typename T>
[[maybe_unused]] static typename std::enable_if<(&T::st_birthtim, true), qint64>::type
birthtime(const T &statBuffer, int)
{ return timespecToMSecs(statBuffer.st_birthtim); }

template <typename T>
[[maybe_unused]] static typename std::enable_if<(&T::st_ctim, true), qint64>::type
ctime(const T &statBuffer, int)
{ return timespecToMSecs(statBuffer.st_ctim); }

template <typename T>
[[maybe_unused]] static typename std::enable_if<(&T::st_mtim, true), qint64>::type
mtime(const T &statBuffer, int)
{ return timespecToMSecs(statBuffer.st_mtim); }

#ifndef st_mtimespec
// Xtimespec
template <typename T>
[[maybe_unused]] static typename std::enable_if<(&T::st_atimespec, true), qint64>::type
atime(const T &statBuffer, int)
{ return timespecToMSecs(statBuffer.st_atimespec); }

template <typename T>
[[maybe_unused]] static typename std::enable_if<(&T::st_birthtimespec, true), qint64>::type
birthtime(const T &statBuffer, int)
{ return timespecToMSecs(statBuffer.st_birthtimespec); }

template <typename T>
[[maybe_unused]] static typename std::enable_if<(&T::st_ctimespec, true), qint64>::type
ctime(const T &statBuffer, int)
{ return timespecToMSecs(statBuffer.st_ctimespec); }

template <typename T>
[[maybe_unused]] static typename std::enable_if<(&T::st_mtimespec, true), qint64>::type
mtime(const T &statBuffer, int)
{ return timespecToMSecs(statBuffer.st_mtimespec); }
#endif

#if !defined(st_mtimensec) && !defined(__alpha__)
// Xtimensec
template <typename T>
[[maybe_unused]] static typename std::enable_if<(&T::st_atimensec, true), qint64>::type
atime(const T &statBuffer, int)
{ return statBuffer.st_atime * Q_INT64_C(1000) + statBuffer.st_atimensec / 1000000; }

template <typename T>
[[maybe_unused]] static typename std::enable_if<(&T::st_birthtimensec, true), qint64>::type
birthtime(const T &statBuffer, int)
{ return statBuffer.st_birthtime * Q_INT64_C(1000) + statBuffer.st_birthtimensec / 1000000; }

template <typename T>
[[maybe_unused]] static typename std::enable_if<(&T::st_ctimensec, true), qint64>::type
ctime(const T &statBuffer, int)
{ return statBuffer.st_ctime * Q_INT64_C(1000) + statBuffer.st_ctimensec / 1000000; }

template <typename T>
[[maybe_unused]] static typename std::enable_if<(&T::st_mtimensec, true), qint64>::type
mtime(const T &statBuffer, int)
{ return statBuffer.st_mtime * Q_INT64_C(1000) + statBuffer.st_mtimensec / 1000000; }
#endif
} // namespace GetFileTimes
} // unnamed namespace

#ifdef STATX_BASIC_STATS
static int qt_real_statx(int fd, const char *pathname, int flags, struct statx *statxBuffer)
{
    unsigned mask = STATX_BASIC_STATS | STATX_BTIME;
    int ret = statx(fd, pathname, flags | AT_NO_AUTOMOUNT, mask, statxBuffer);
    return ret == -1 ? -errno : 0;
}

static int qt_statx(const char *pathname, struct statx *statxBuffer)
{
    return qt_real_statx(AT_FDCWD, pathname, 0, statxBuffer);
}

static int qt_lstatx(const char *pathname, struct statx *statxBuffer)
{
    return qt_real_statx(AT_FDCWD, pathname, AT_SYMLINK_NOFOLLOW, statxBuffer);
}

static int qt_fstatx(int fd, struct statx *statxBuffer)
{
    return qt_real_statx(fd, "", AT_EMPTY_PATH, statxBuffer);
}

inline void QFileSystemMetaData::fillFromStatxBuf(const struct statx &statxBuffer)
{
    // Permissions
    if (statxBuffer.stx_mode & S_IRUSR)
        entryFlags |= QFileSystemMetaData::OwnerReadPermission;
    if (statxBuffer.stx_mode & S_IWUSR)
        entryFlags |= QFileSystemMetaData::OwnerWritePermission;
    if (statxBuffer.stx_mode & S_IXUSR)
        entryFlags |= QFileSystemMetaData::OwnerExecutePermission;

    if (statxBuffer.stx_mode & S_IRGRP)
        entryFlags |= QFileSystemMetaData::GroupReadPermission;
    if (statxBuffer.stx_mode & S_IWGRP)
        entryFlags |= QFileSystemMetaData::GroupWritePermission;
    if (statxBuffer.stx_mode & S_IXGRP)
        entryFlags |= QFileSystemMetaData::GroupExecutePermission;

    if (statxBuffer.stx_mode & S_IROTH)
        entryFlags |= QFileSystemMetaData::OtherReadPermission;
    if (statxBuffer.stx_mode & S_IWOTH)
        entryFlags |= QFileSystemMetaData::OtherWritePermission;
    if (statxBuffer.stx_mode & S_IXOTH)
        entryFlags |= QFileSystemMetaData::OtherExecutePermission;

    // Type
    if (S_ISLNK(statxBuffer.stx_mode))
        entryFlags |= QFileSystemMetaData::LinkType;
    if ((statxBuffer.stx_mode & S_IFMT) == S_IFREG)
        entryFlags |= QFileSystemMetaData::FileType;
    else if ((statxBuffer.stx_mode & S_IFMT) == S_IFDIR)
        entryFlags |= QFileSystemMetaData::DirectoryType;
    else if ((statxBuffer.stx_mode & S_IFMT) != S_IFBLK)
        entryFlags |= QFileSystemMetaData::SequentialType;

    // Attributes
    entryFlags |= QFileSystemMetaData::ExistsAttribute; // inode exists
    if (statxBuffer.stx_nlink == 0)
        entryFlags |= QFileSystemMetaData::WasDeletedAttribute;
    size_ = qint64(statxBuffer.stx_size);

    // Times
    auto toMSecs = [](struct statx_timestamp ts)
    {
        return qint64(ts.tv_sec) * 1000 + (ts.tv_nsec / 1000000);
    };
    accessTime_ = toMSecs(statxBuffer.stx_atime);
    metadataChangeTime_ = toMSecs(statxBuffer.stx_ctime);
    modificationTime_ = toMSecs(statxBuffer.stx_mtime);
    if (statxBuffer.stx_mask & STATX_BTIME)
        birthTime_ = toMSecs(statxBuffer.stx_btime);
    else
        birthTime_ = 0;

    userId_ = statxBuffer.stx_uid;
    groupId_ = statxBuffer.stx_gid;
}
#else
static int qt_statx(const char *, struct statx *)
{ return -ENOSYS; }

static int qt_lstatx(const char *, struct statx *)
{ return -ENOSYS; }

static int qt_fstatx(int, struct statx *)
{ return -ENOSYS; }

inline void QFileSystemMetaData::fillFromStatxBuf(const struct statx &)
{ }
#endif

//static
bool QFileSystemEngine::fillMetaData(int fd, QFileSystemMetaData &data)
{
    data.entryFlags &= ~QFileSystemMetaData::PosixStatFlags;
    data.knownFlagsMask |= QFileSystemMetaData::PosixStatFlags;

    struct statx statxBuffer;

    int ret = qt_fstatx(fd, &statxBuffer);
    if (ret != -ENOSYS) {
        if (ret == 0) {
            data.fillFromStatxBuf(statxBuffer);
            return true;
        }
        return false;
    }

    QT_STATBUF statBuffer;

    if (QT_FSTAT(fd, &statBuffer) == 0) {
        data.fillFromStatBuf(statBuffer);
        return true;
    }

    return false;
}

#if defined(_DEXTRA_FIRST)
static void fillStat64fromStat32(struct stat64 *statBuf64, const struct stat &statBuf32)
{
    statBuf64->st_mode = statBuf32.st_mode;
    statBuf64->st_size = statBuf32.st_size;
#if _POSIX_VERSION >= 200809L
    statBuf64->st_ctim = statBuf32.st_ctim;
    statBuf64->st_mtim = statBuf32.st_mtim;
    statBuf64->st_atim = statBuf32.st_atim;
#else
    statBuf64->st_ctime = statBuf32.st_ctime;
    statBuf64->st_mtime = statBuf32.st_mtime;
    statBuf64->st_atime = statBuf32.st_atime;
#endif
    statBuf64->st_uid = statBuf32.st_uid;
    statBuf64->st_gid = statBuf32.st_gid;
}
#endif

void QFileSystemMetaData::fillFromStatBuf(const QT_STATBUF &statBuffer)
{
    // Permissions
    if (statBuffer.st_mode & S_IRUSR)
        entryFlags |= QFileSystemMetaData::OwnerReadPermission;
    if (statBuffer.st_mode & S_IWUSR)
        entryFlags |= QFileSystemMetaData::OwnerWritePermission;
    if (statBuffer.st_mode & S_IXUSR)
        entryFlags |= QFileSystemMetaData::OwnerExecutePermission;

    if (statBuffer.st_mode & S_IRGRP)
        entryFlags |= QFileSystemMetaData::GroupReadPermission;
    if (statBuffer.st_mode & S_IWGRP)
        entryFlags |= QFileSystemMetaData::GroupWritePermission;
    if (statBuffer.st_mode & S_IXGRP)
        entryFlags |= QFileSystemMetaData::GroupExecutePermission;

    if (statBuffer.st_mode & S_IROTH)
        entryFlags |= QFileSystemMetaData::OtherReadPermission;
    if (statBuffer.st_mode & S_IWOTH)
        entryFlags |= QFileSystemMetaData::OtherWritePermission;
    if (statBuffer.st_mode & S_IXOTH)
        entryFlags |= QFileSystemMetaData::OtherExecutePermission;

    // Type
    if ((statBuffer.st_mode & S_IFMT) == S_IFREG)
        entryFlags |= QFileSystemMetaData::FileType;
    else if ((statBuffer.st_mode & S_IFMT) == S_IFDIR)
        entryFlags |= QFileSystemMetaData::DirectoryType;
    else if ((statBuffer.st_mode & S_IFMT) != S_IFBLK)
        entryFlags |= QFileSystemMetaData::SequentialType;

    // Attributes
    entryFlags |= QFileSystemMetaData::ExistsAttribute; // inode exists
    if (statBuffer.st_nlink == 0)
        entryFlags |= QFileSystemMetaData::WasDeletedAttribute;
    size_ = statBuffer.st_size;
#ifdef UF_HIDDEN
    if (statBuffer.st_flags & UF_HIDDEN) {
        entryFlags |= QFileSystemMetaData::HiddenAttribute;
        knownFlagsMask |= QFileSystemMetaData::HiddenAttribute;
    }
#endif

    // Times
    accessTime_ = GetFileTimes::atime(statBuffer, 0);
    birthTime_ = GetFileTimes::birthtime(statBuffer, 0);
    metadataChangeTime_ = GetFileTimes::ctime(statBuffer, 0);
    modificationTime_ = GetFileTimes::mtime(statBuffer, 0);

    userId_ = statBuffer.st_uid;
    groupId_ = statBuffer.st_gid;
}

void QFileSystemMetaData::fillFromDirEnt(const QT_DIRENT &entry)
{
#if defined(_DEXTRA_FIRST)
    knownFlagsMask = {};
    entryFlags = {};
    for (dirent_extra *extra = _DEXTRA_FIRST(&entry); _DEXTRA_VALID(extra, &entry);
         extra = _DEXTRA_NEXT(extra)) {
        if (extra->d_type == _DTYPE_STAT || extra->d_type == _DTYPE_LSTAT) {

            const struct dirent_extra_stat * const extra_stat =
                    reinterpret_cast<struct dirent_extra_stat *>(extra);

            // Remember whether this was a link or not, this saves an lstat() call later.
            if (extra->d_type == _DTYPE_LSTAT) {
                knownFlagsMask |= QFileSystemMetaData::LinkType;
                if (S_ISLNK(extra_stat->d_stat.st_mode))
                    entryFlags |= QFileSystemMetaData::LinkType;
            }

            // For symlinks, the extra type _DTYPE_LSTAT doesn't work for filling out the meta data,
            // as we need the stat() information there, not the lstat() information.
            // In this case, don't use the extra information.
            // Unfortunately, readdir() never seems to return extra info of type _DTYPE_STAT, so for
            // symlinks, we always incur the cost of an extra stat() call later.
            if (S_ISLNK(extra_stat->d_stat.st_mode) && extra->d_type == _DTYPE_LSTAT)
                continue;

#if defined(QT_USE_XOPEN_LFS_EXTENSIONS) && defined(QT_LARGEFILE_SUPPORT)
            // Even with large file support, d_stat is always of type struct stat, not struct stat64,
            // so it needs to be converted
            struct stat64 statBuf;
            fillStat64fromStat32(&statBuf, extra_stat->d_stat);
            fillFromStatBuf(statBuf);
#else
            fillFromStatBuf(extra_stat->d_stat);
#endif
            knownFlagsMask |= QFileSystemMetaData::PosixStatFlags;
            if (!S_ISLNK(extra_stat->d_stat.st_mode)) {
                knownFlagsMask |= QFileSystemMetaData::ExistsAttribute;
                entryFlags |= QFileSystemMetaData::ExistsAttribute;
            }
        }
    }
#elif defined(_DIRENT_HAVE_D_TYPE) || defined(Q_OS_BSD4)
    // BSD4 includes OS X and iOS

    // ### This will clear all entry flags and knownFlagsMask
    switch (entry.d_type)
    {
    case DT_DIR:
        knownFlagsMask = QFileSystemMetaData::LinkType
            | QFileSystemMetaData::FileType
            | QFileSystemMetaData::DirectoryType
            | QFileSystemMetaData::SequentialType
            | QFileSystemMetaData::ExistsAttribute;

        entryFlags = QFileSystemMetaData::DirectoryType
            | QFileSystemMetaData::ExistsAttribute;

        break;

    case DT_BLK:
        knownFlagsMask = QFileSystemMetaData::LinkType
            | QFileSystemMetaData::FileType
            | QFileSystemMetaData::DirectoryType
            | QFileSystemMetaData::BundleType
            | QFileSystemMetaData::AliasType
            | QFileSystemMetaData::SequentialType
            | QFileSystemMetaData::ExistsAttribute;

        entryFlags = QFileSystemMetaData::ExistsAttribute;

        break;

    case DT_CHR:
    case DT_FIFO:
    case DT_SOCK:
        // ### System attribute
        knownFlagsMask = QFileSystemMetaData::LinkType
            | QFileSystemMetaData::FileType
            | QFileSystemMetaData::DirectoryType
            | QFileSystemMetaData::BundleType
            | QFileSystemMetaData::AliasType
            | QFileSystemMetaData::SequentialType
            | QFileSystemMetaData::ExistsAttribute;

        entryFlags = QFileSystemMetaData::SequentialType
            | QFileSystemMetaData::ExistsAttribute;

        break;

    case DT_LNK:
        knownFlagsMask = QFileSystemMetaData::LinkType;
        entryFlags = QFileSystemMetaData::LinkType;
        break;

    case DT_REG:
        knownFlagsMask = QFileSystemMetaData::LinkType
            | QFileSystemMetaData::FileType
            | QFileSystemMetaData::DirectoryType
            | QFileSystemMetaData::BundleType
            | QFileSystemMetaData::SequentialType
            | QFileSystemMetaData::ExistsAttribute;

        entryFlags = QFileSystemMetaData::FileType
            | QFileSystemMetaData::ExistsAttribute;

        break;

    case DT_UNKNOWN:
    default:
        clear();
    }
#else
    Q_UNUSED(entry);
#endif
}

//static
QFileSystemEntry QFileSystemEngine::getLinkTarget(const QFileSystemEntry &link, QFileSystemMetaData &data)
{
    Q_CHECK_FILE_NAME(link, link);

    QByteArray s = qt_readlink(link.nativeFilePath().constData());
    if (s.length() > 0) {
        QString ret;
        if (!data.hasFlags(QFileSystemMetaData::DirectoryType))
            fillMetaData(link, data, QFileSystemMetaData::DirectoryType);
        if (data.isDirectory() && s[0] != '/') {
            QDir parent(link.filePath());
            parent.cdUp();
            ret = parent.path();
            if (!ret.isEmpty() && !ret.endsWith(QLatin1Char('/')))
                ret += QLatin1Char('/');
        }
        ret += QFile::decodeName(s);

        if (!ret.startsWith(QLatin1Char('/')))
            ret.prepend(absoluteName(link).path() + QLatin1Char('/'));
        ret = QDir::cleanPath(ret);
        if (ret.size() > 1 && ret.endsWith(QLatin1Char('/')))
            ret.chop(1);
        return QFileSystemEntry(ret);
    }
#if defined(Q_OS_DARWIN)
    {
        QCFString path = CFStringCreateWithFileSystemRepresentation(0,
            QFile::encodeName(QDir::cleanPath(link.filePath())).data());
        if (!path)
            return QFileSystemEntry();

        QCFType<CFURLRef> url = CFURLCreateWithFileSystemPath(0, path, kCFURLPOSIXPathStyle,
            data.hasFlags(QFileSystemMetaData::DirectoryType));
        if (!url)
            return QFileSystemEntry();

        QCFType<CFDataRef> bookmarkData = CFURLCreateBookmarkDataFromFile(0, url, NULL);
        if (!bookmarkData)
            return QFileSystemEntry();

        QCFType<CFURLRef> resolvedUrl = CFURLCreateByResolvingBookmarkData(0,
            bookmarkData,
            (CFURLBookmarkResolutionOptions)(kCFBookmarkResolutionWithoutUIMask
                | kCFBookmarkResolutionWithoutMountingMask), NULL, NULL, NULL, NULL);
        if (!resolvedUrl)
            return QFileSystemEntry();

        QCFString cfstr(CFURLCopyFileSystemPath(resolvedUrl, kCFURLPOSIXPathStyle));
        if (!cfstr)
            return QFileSystemEntry();

        return QFileSystemEntry(QString::fromCFString(cfstr));
    }
#endif
    return QFileSystemEntry();
}

//static
QFileSystemEntry QFileSystemEngine::canonicalName(const QFileSystemEntry &entry, QFileSystemMetaData &data)
{
    Q_CHECK_FILE_NAME(entry, entry);

#if !defined(Q_OS_MAC) && !defined(Q_OS_QNX) && !defined(Q_OS_ANDROID) && !defined(Q_OS_HAIKU) && _POSIX_VERSION < 200809L
    // realpath(X,0) is not supported
    Q_UNUSED(data);
    return QFileSystemEntry(slowCanonicalized(absoluteName(entry).filePath()));
#else
    char stack_result[PATH_MAX+1];
    char *resolved_name = nullptr;
# if defined(Q_OS_DARWIN) || defined(Q_OS_ANDROID)
    // On some Android and macOS versions, realpath() will return a path even if
    // it does not exist. To work around this, we check existence in advance.
    if (!data.hasFlags(QFileSystemMetaData::ExistsAttribute))
        fillMetaData(entry, data, QFileSystemMetaData::ExistsAttribute);

    if (!data.exists()) {
        errno = ENOENT;
    } else {
        resolved_name = stack_result;
    }
    if (resolved_name && realpath(entry.nativeFilePath().constData(), resolved_name) == nullptr)
        resolved_name = nullptr;
# else
#  if _POSIX_VERSION >= 200801L // ask realpath to allocate memory
    resolved_name = realpath(entry.nativeFilePath().constData(), nullptr);
#  else
    resolved_name = stack_result;
    if (realpath(entry.nativeFilePath().constData(), resolved_name) == nullptr)
        resolved_name = nullptr;
#  endif
# endif
    if (resolved_name) {
        data.knownFlagsMask |= QFileSystemMetaData::ExistsAttribute;
        data.entryFlags |= QFileSystemMetaData::ExistsAttribute;
        QString canonicalPath = QDir::cleanPath(QFile::decodeName(resolved_name));
        if (resolved_name != stack_result)
            free(resolved_name);
        return QFileSystemEntry(canonicalPath);
    } else if (errno == ENOENT || errno == ENOTDIR) { // file doesn't exist
        data.knownFlagsMask |= QFileSystemMetaData::ExistsAttribute;
        data.entryFlags &= ~(QFileSystemMetaData::ExistsAttribute);
        return QFileSystemEntry();
    }
    return entry;
#endif
}

//static
QFileSystemEntry QFileSystemEngine::absoluteName(const QFileSystemEntry &entry)
{
    Q_CHECK_FILE_NAME(entry, entry);

    if (entry.isAbsolute() && entry.isClean())
        return entry;

    QByteArray orig = entry.nativeFilePath();
    QByteArray result;
    if (orig.isEmpty() || !orig.startsWith('/')) {
        QFileSystemEntry cur(currentPath());
        result = cur.nativeFilePath();
    }
    if (!orig.isEmpty() && !(orig.length() == 1 && orig[0] == '.')) {
        if (!result.isEmpty() && !result.endsWith('/'))
            result.append('/');
        result.append(orig);
    }

    if (result.length() == 1 && result[0] == '/')
        return QFileSystemEntry(result, QFileSystemEntry::FromNativePath());
    const bool isDir = result.endsWith('/');

    /* as long as QDir::cleanPath() operates on a QString we have to convert to a string here.
     * ideally we never convert to a string since that loses information. Please fix after
     * we get a QByteArray version of QDir::cleanPath()
     */
    QFileSystemEntry resultingEntry(result, QFileSystemEntry::FromNativePath());
    QString stringVersion = QDir::cleanPath(resultingEntry.filePath());
    if (isDir)
        stringVersion.append(QLatin1Char('/'));
    return QFileSystemEntry(stringVersion);
}

//static
QByteArray QFileSystemEngine::id(const QFileSystemEntry &entry)
{
    Q_CHECK_FILE_NAME(entry, QByteArray());

    QT_STATBUF statResult;
    if (QT_STAT(entry.nativeFilePath().constData(), &statResult)) {
        if (errno != ENOENT)
            qErrnoWarning("stat() failed for '%s'", entry.nativeFilePath().constData());
        return QByteArray();
    }
    QByteArray result = QByteArray::number(quint64(statResult.st_dev), 16);
    result += ':';
    result += QByteArray::number(quint64(statResult.st_ino));
    return result;
}

//static
QByteArray QFileSystemEngine::id(int fd)
{
    QT_STATBUF statResult;
    if (QT_FSTAT(fd, &statResult)) {
        qErrnoWarning("fstat() failed for fd %d", fd);
        return QByteArray();
    }
    QByteArray result = QByteArray::number(quint64(statResult.st_dev), 16);
    result += ':';
    result += QByteArray::number(quint64(statResult.st_ino));
    return result;
}

//static
QString QFileSystemEngine::resolveUserName(uint userId)
{
#if QT_CONFIG(thread) && defined(_POSIX_THREAD_SAFE_FUNCTIONS) && !defined(Q_OS_OPENBSD)
    int size_max = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (size_max == -1)
        size_max = 1024;
    QVarLengthArray<char, 1024> buf(size_max);
#endif

#if !defined(Q_OS_INTEGRITY) && !defined(Q_OS_WASM)
    struct passwd *pw = nullptr;
#if QT_CONFIG(thread) && defined(_POSIX_THREAD_SAFE_FUNCTIONS) && !defined(Q_OS_OPENBSD) && !defined(Q_OS_VXWORKS)
    struct passwd entry;
    getpwuid_r(userId, &entry, buf.data(), buf.size(), &pw);
#else
    pw = getpwuid(userId);
#endif
    if (pw)
        return QFile::decodeName(QByteArray(pw->pw_name));
#else // Integrity || WASM
    Q_UNUSED(userId);
#endif
    return QString();
}

//static
QString QFileSystemEngine::resolveGroupName(uint groupId)
{
#if QT_CONFIG(thread) && defined(_POSIX_THREAD_SAFE_FUNCTIONS) && !defined(Q_OS_OPENBSD)
    int size_max = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (size_max == -1)
        size_max = 1024;
    QVarLengthArray<char, 1024> buf(size_max);
#endif

#if !defined(Q_OS_INTEGRITY) && !defined(Q_OS_WASM)
    struct group *gr = nullptr;
#if QT_CONFIG(thread) && defined(_POSIX_THREAD_SAFE_FUNCTIONS) && !defined(Q_OS_OPENBSD) && !defined(Q_OS_VXWORKS) && (!defined(Q_OS_ANDROID) || defined(Q_OS_ANDROID) && (__ANDROID_API__ >= 24))
    size_max = sysconf(_SC_GETGR_R_SIZE_MAX);
    if (size_max == -1)
        size_max = 1024;
    buf.resize(size_max);
    struct group entry;
    // Some large systems have more members than the POSIX max size
    // Loop over by doubling the buffer size (upper limit 250k)
    for (unsigned size = size_max; size < 256000; size += size)
    {
        buf.resize(size);
        // ERANGE indicates that the buffer was too small
        if (!getgrgid_r(groupId, &entry, buf.data(), buf.size(), &gr)
            || errno != ERANGE)
            break;
    }
#else
    gr = getgrgid(groupId);
#endif
    if (gr)
        return QFile::decodeName(QByteArray(gr->gr_name));
#else // Integrity || WASM
    Q_UNUSED(groupId);
#endif
    return QString();
}

#if defined(Q_OS_DARWIN)
//static
QString QFileSystemEngine::bundleName(const QFileSystemEntry &entry)
{
    QCFType<CFURLRef> url = CFURLCreateWithFileSystemPath(0, QCFString(entry.filePath()),
            kCFURLPOSIXPathStyle, true);
    if (QCFType<CFDictionaryRef> dict = CFBundleCopyInfoDictionaryForURL(url)) {
        if (CFTypeRef name = (CFTypeRef)CFDictionaryGetValue(dict, kCFBundleNameKey)) {
            if (CFGetTypeID(name) == CFStringGetTypeID())
                return QString::fromCFString((CFStringRef)name);
        }
    }
    return QString();
}
#endif

//static
bool QFileSystemEngine::fillMetaData(const QFileSystemEntry &entry, QFileSystemMetaData &data,
        QFileSystemMetaData::MetaDataFlags what)
{
    Q_CHECK_FILE_NAME(entry, false);

#if defined(Q_OS_DARWIN)
    if (what & QFileSystemMetaData::BundleType) {
        if (!data.hasFlags(QFileSystemMetaData::DirectoryType))
            what |= QFileSystemMetaData::DirectoryType;
    }
#endif
#ifdef UF_HIDDEN
    if (what & QFileSystemMetaData::HiddenAttribute) {
        // OS X >= 10.5: st_flags & UF_HIDDEN
        what |= QFileSystemMetaData::PosixStatFlags;
    }
#endif // defined(Q_OS_DARWIN)

    // if we're asking for any of the stat(2) flags, then we're getting them all
    if (what & QFileSystemMetaData::PosixStatFlags)
        what |= QFileSystemMetaData::PosixStatFlags;

    data.entryFlags &= ~what;

    const QByteArray nativeFilePath = entry.nativeFilePath();
    int entryErrno = 0; // innocent until proven otherwise

    // first, we may try lstat(2). Possible outcomes:
    //  - success and is a symlink: filesystem entry exists, but we need stat(2)
    //    -> statResult = -1;
    //  - success and is not a symlink: filesystem entry exists and we're done
    //    -> statResult = 0
    //  - failure: really non-existent filesystem entry
    //    -> entryExists = false; statResult = 0;
    //    both stat(2) and lstat(2) may generate a number of different errno
    //    conditions, but of those, the only ones that could happen and the
    //    entry still exist are EACCES, EFAULT, ENOMEM and EOVERFLOW. If we get
    //    EACCES or ENOMEM, then we have no choice on how to proceed, so we may
    //    as well conclude it doesn't exist; EFAULT can't happen and EOVERFLOW
    //    shouldn't happen because we build in _LARGEFIE64.
    union {
        QT_STATBUF statBuffer;
        struct statx statxBuffer;
    };
    int statResult = -1;
    if (what & QFileSystemMetaData::LinkType) {
        mode_t mode = 0;
        statResult = qt_lstatx(nativeFilePath, &statxBuffer);
        if (statResult == -ENOSYS) {
            // use lstst(2)
            statResult = QT_LSTAT(nativeFilePath, &statBuffer);
            if (statResult == 0)
                mode = statBuffer.st_mode;
        } else if (statResult == 0) {
            statResult = 1; // record it was statx(2) that succeeded
            mode = statxBuffer.stx_mode;
        }

        if (statResult >= 0) {
            if (S_ISLNK(mode)) {
               // it's a symlink, we don't know if the file "exists"
                data.entryFlags |= QFileSystemMetaData::LinkType;
                statResult = -1;    // force stat(2) below
            } else {
                // it's a reagular file and it exists
                if (statResult)
                    data.fillFromStatxBuf(statxBuffer);
                else
                    data.fillFromStatBuf(statBuffer);
                data.knownFlagsMask |= QFileSystemMetaData::PosixStatFlags
                        | QFileSystemMetaData::ExistsAttribute;
                data.entryFlags |= QFileSystemMetaData::ExistsAttribute;
            }
        } else {
            // it doesn't exist
            entryErrno = errno;
            data.knownFlagsMask |= QFileSystemMetaData::ExistsAttribute;
        }

        data.knownFlagsMask |= QFileSystemMetaData::LinkType;
    }

    // second, we try a regular stat(2)
    if (statResult == -1 && (what & QFileSystemMetaData::PosixStatFlags)) {
        if (entryErrno == 0 && statResult == -1) {
            data.entryFlags &= ~QFileSystemMetaData::PosixStatFlags;
            statResult = qt_statx(nativeFilePath, &statxBuffer);
            if (statResult == -ENOSYS) {
                // use stat(2)
                statResult = QT_STAT(nativeFilePath, &statBuffer);
                if (statResult == 0)
                    data.fillFromStatBuf(statBuffer);
            } else if (statResult == 0) {
                data.fillFromStatxBuf(statxBuffer);
            }
        }

        if (statResult != 0) {
            entryErrno = errno;
            data.birthTime_ = 0;
            data.metadataChangeTime_ = 0;
            data.modificationTime_ = 0;
            data.accessTime_ = 0;
            data.size_ = 0;
            data.userId_ = (uint) -2;
            data.groupId_ = (uint) -2;
        }

        // reset the mask
        data.knownFlagsMask |= QFileSystemMetaData::PosixStatFlags
            | QFileSystemMetaData::ExistsAttribute;
    }

    // third, we try access(2)
    if (what & (QFileSystemMetaData::UserPermissions | QFileSystemMetaData::ExistsAttribute)) {
        // calculate user permissions
        auto checkAccess = [&](QFileSystemMetaData::MetaDataFlag flag, int mode) {
            if (entryErrno != 0 || (what & flag) == 0)
                return;
            if (QT_ACCESS(nativeFilePath, mode) == 0) {
                // access ok (and file exists)
                data.entryFlags |= flag | QFileSystemMetaData::ExistsAttribute;
            } else if (errno != EACCES && errno != EROFS) {
                entryErrno = errno;
            }
        };

        checkAccess(QFileSystemMetaData::UserReadPermission, R_OK);
        checkAccess(QFileSystemMetaData::UserWritePermission, W_OK);
        checkAccess(QFileSystemMetaData::UserExecutePermission, X_OK);

        // if we still haven't found out if the file exists, try F_OK
        if (entryErrno == 0 && (data.entryFlags & QFileSystemMetaData::ExistsAttribute) == 0) {
            if (QT_ACCESS(nativeFilePath, F_OK) == -1)
                entryErrno = errno;
            else
                data.entryFlags |= QFileSystemMetaData::ExistsAttribute;
        }

        data.knownFlagsMask |= (what & QFileSystemMetaData::UserPermissions) |
                QFileSystemMetaData::ExistsAttribute;
    }

#if defined(Q_OS_DARWIN)
    if (what & QFileSystemMetaData::AliasType) {
        if (entryErrno == 0 && hasResourcePropertyFlag(data, entry, kCFURLIsAliasFileKey))
            data.entryFlags |= QFileSystemMetaData::AliasType;
        data.knownFlagsMask |= QFileSystemMetaData::AliasType;
    }

    if (what & QFileSystemMetaData::BundleType) {
        if (entryErrno == 0 && isPackage(data, entry))
            data.entryFlags |= QFileSystemMetaData::BundleType;

        data.knownFlagsMask |= QFileSystemMetaData::BundleType;
    }
#endif

    if (what & QFileSystemMetaData::HiddenAttribute
            && !data.isHidden()) {
        QString fileName = entry.fileName();
        if ((fileName.size() > 0 && fileName.at(0) == QLatin1Char('.'))
#if defined(Q_OS_DARWIN)
                || (entryErrno == 0 && hasResourcePropertyFlag(data, entry, kCFURLIsHiddenKey))
#endif
                )
            data.entryFlags |= QFileSystemMetaData::HiddenAttribute;
        data.knownFlagsMask |= QFileSystemMetaData::HiddenAttribute;
    }

    if (entryErrno != 0) {
        what &= ~QFileSystemMetaData::LinkType; // don't clear link: could be broken symlink
        data.clearFlags(what);
        return false;
    }
    return true;
}

// static
bool QFileSystemEngine::cloneFile(int srcfd, int dstfd, const QFileSystemMetaData &knownData)
{
    QT_STATBUF statBuffer;
    if (knownData.hasFlags(QFileSystemMetaData::PosixStatFlags) &&
            knownData.isFile()) {
        statBuffer.st_mode = S_IFREG;
    } else if (knownData.hasFlags(QFileSystemMetaData::PosixStatFlags) &&
               knownData.isDirectory()) {
        return false;   // fcopyfile(3) returns success on directories
    } else if (QT_FSTAT(srcfd, &statBuffer) == -1) {
        return false;
    } else if (!S_ISREG((statBuffer.st_mode))) {
        // not a regular file, let QFile do the copy
        return false;
    }

#if defined(Q_OS_LINUX)
    // first, try FICLONE (only works on regular files and only on certain fs)
    if (::ioctl(dstfd, FICLONE, srcfd) == 0)
        return true;

    // Second, try sendfile (it can send to some special types too).
    // sendfile(2) is limited in the kernel to 2G - 4k
    const size_t SendfileSize = 0x7ffff000;

    ssize_t n = ::sendfile(dstfd, srcfd, nullptr, SendfileSize);
    if (n == -1) {
        // if we got an error here, give up and try at an upper layer
        return false;
    }

    while (n) {
        n = ::sendfile(dstfd, srcfd, nullptr, SendfileSize);
        if (n == -1) {
            // uh oh, this is probably a real error (like ENOSPC), but we have
            // no way to notify QFile of partial success, so just erase any work
            // done (hopefully we won't get any errors, because there's nothing
            // we can do about them)
            n = ftruncate(dstfd, 0);
            n = lseek(srcfd, 0, SEEK_SET);
            n = lseek(dstfd, 0, SEEK_SET);
            return false;
        }
    }

    return true;
#elif defined(Q_OS_DARWIN)
    // try fcopyfile
    return fcopyfile(srcfd, dstfd, nullptr, COPYFILE_DATA | COPYFILE_STAT) == 0;
#else
    Q_UNUSED(dstfd);
    return false;
#endif
}

// Note: if \a shouldMkdirFirst is false, we assume the caller did try to mkdir
// before calling this function.
static bool createDirectoryWithParents(const QByteArray &nativeName, mode_t mode,
                                       bool shouldMkdirFirst = true)
{
    // helper function to check if a given path is a directory, since mkdir can
    // fail if the dir already exists (it may have been created by another
    // thread or another process)
    const auto isDir = [](const QByteArray &nativeName) {
        QT_STATBUF st;
        return QT_STAT(nativeName.constData(), &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
    };

    if (shouldMkdirFirst && QT_MKDIR(nativeName, mode) == 0)
        return true;
    if (errno == EISDIR)
        return true;
    if (errno == EEXIST)
        return isDir(nativeName);
    if (errno != ENOENT)
        return false;

    // mkdir failed because the parent dir doesn't exist, so try to create it
    int slash = nativeName.lastIndexOf('/');
    if (slash < 1)
        return false;

    QByteArray parentNativeName = nativeName.left(slash);
    if (!createDirectoryWithParents(parentNativeName, mode))
        return false;

    // try again
    if (QT_MKDIR(nativeName, mode) == 0)
        return true;
    return errno == EEXIST && isDir(nativeName);
}

//static
bool QFileSystemEngine::createDirectory(const QFileSystemEntry &entry, bool createParents,
                                        std::optional<QFile::Permissions> permissions)
{
    QString dirName = entry.filePath();
    Q_CHECK_FILE_NAME(dirName, false);

    // Darwin doesn't support trailing /'s, so remove for everyone
    while (dirName.size() > 1 && dirName.endsWith(QLatin1Char('/')))
        dirName.chop(1);

    // try to mkdir this directory
    QByteArray nativeName = QFile::encodeName(dirName);
    mode_t mode = permissions ? QtPrivate::toMode_t(*permissions) : 0777;
    if (QT_MKDIR(nativeName, mode) == 0)
        return true;
    if (!createParents)
        return false;

    return createDirectoryWithParents(nativeName, mode, false);
}

//static
bool QFileSystemEngine::removeDirectory(const QFileSystemEntry &entry, bool removeEmptyParents)
{
    Q_CHECK_FILE_NAME(entry, false);

    if (removeEmptyParents) {
        QString dirName = QDir::cleanPath(entry.filePath());
        for (int oldslash = 0, slash=dirName.length(); slash > 0; oldslash = slash) {
            const QByteArray chunk = QFile::encodeName(dirName.left(slash));
            QT_STATBUF st;
            if (QT_STAT(chunk.constData(), &st) != -1) {
                if ((st.st_mode & S_IFMT) != S_IFDIR)
                    return false;
                if (::rmdir(chunk.constData()) != 0)
                    return oldslash != 0;
            } else {
                return false;
            }
            slash = dirName.lastIndexOf(QDir::separator(), oldslash-1);
        }
        return true;
    }
    return rmdir(QFile::encodeName(entry.filePath()).constData()) == 0;
}

//static
bool QFileSystemEngine::createLink(const QFileSystemEntry &source, const QFileSystemEntry &target, QSystemError &error)
{
    Q_CHECK_FILE_NAME(source, false);
    Q_CHECK_FILE_NAME(target, false);

    if (::symlink(source.nativeFilePath().constData(), target.nativeFilePath().constData()) == 0)
        return true;
    error = QSystemError(errno, QSystemError::StandardLibraryError);
    return false;
}

#ifndef Q_OS_DARWIN
/*
    Implementing as per https://specifications.freedesktop.org/trash-spec/trashspec-1.0.html
*/

// bootstrapped tools don't need this, and we don't want QStorageInfo
#ifndef QT_BOOTSTRAPPED
static QString freeDesktopTrashLocation(const QString &sourcePath)
{
    auto makeTrashDir = [](const QDir &topDir, const QString &trashDir) -> QString {
        auto ownerPerms = QFileDevice::ReadOwner
                        | QFileDevice::WriteOwner
                        | QFileDevice::ExeOwner;
        QString targetDir = topDir.filePath(trashDir);
        // deliberately not using mkpath, since we want to fail if topDir doesn't exist
        if (topDir.mkdir(trashDir))
            QFile::setPermissions(targetDir, ownerPerms);
        if (QFileInfo(targetDir).isDir())
            return targetDir;
        return QString();
    };
    auto isSticky = [](const QFileInfo &fileInfo) -> bool {
        struct stat st;
        if (stat(QFile::encodeName(fileInfo.absoluteFilePath()).constData(), &st) == 0)
            return st.st_mode & S_ISVTX;

        return false;
    };

    QString trash;
    const QStorageInfo sourceStorage(sourcePath);
    const QStorageInfo homeStorage(QDir::home());
    // We support trashing of files outside the users home partition
    if (sourceStorage != homeStorage) {
        const QLatin1String dotTrash(".Trash");
        QDir topDir(sourceStorage.rootPath());
        /*
            Method 1:
            "An administrator can create an $topdir/.Trash directory. The permissions on this
            directories should permit all users who can trash files at all to write in it;
            and the “sticky bit” in the permissions must be set, if the file system supports
            it.
            When trashing a file from a non-home partition/device, an implementation
            (if it supports trashing in top directories) MUST check for the presence
            of $topdir/.Trash."
        */
        const QString userID = QString::number(::getuid());
        if (topDir.cd(dotTrash)) {
            const QFileInfo trashInfo(topDir.path());

            // we MUST check that the sticky bit is set, and that it is not a symlink
            if (trashInfo.isSymLink()) {
                // we SHOULD report the failed check to the administrator
                qCritical("Warning: '%s' is a symlink to '%s'",
                          trashInfo.absoluteFilePath().toLocal8Bit().constData(),
                          trashInfo.symLinkTarget().toLatin1().constData());
            } else if (!isSticky(trashInfo)) {
                // we SHOULD report the failed check to the administrator
                qCritical("Warning: '%s' doesn't have sticky bit set!",
                          trashInfo.absoluteFilePath().toLocal8Bit().constData());
            } else if (trashInfo.isDir()) {
                /*
                    "If the directory exists and passes the checks, a subdirectory of the
                     $topdir/.Trash directory is to be used as the user's trash directory
                     for this partition/device. The name of this subdirectory is the numeric
                     identifier of the current user ($topdir/.Trash/$uid).
                     When trashing a file, if this directory does not exist for the current user,
                     the implementation MUST immediately create it, without any warnings or
                     delays for the user."
                */
                trash = makeTrashDir(topDir, userID);
            }
        }
        /*
            Method 2:
            "If an $topdir/.Trash directory is absent, an $topdir/.Trash-$uid directory is to be
             used as the user's trash directory for this device/partition. [...] When trashing a
             file, if an $topdir/.Trash-$uid directory does not exist, the implementation MUST
             immediately create it, without any warnings or delays for the user."
        */
        if (trash.isEmpty()) {
            topDir = QDir(sourceStorage.rootPath());
            const QString userTrashDir = dotTrash + QLatin1Char('-') + userID;
            trash = makeTrashDir(topDir, userTrashDir);
        }
    }
    /*
        "If both (1) and (2) fail [...], the implementation MUST either trash the
         file into the user's “home trash” or refuse to trash it."

         We trash the file into the user's home trash.

        "Its name and location are $XDG_DATA_HOME/Trash"; $XDG_DATA_HOME is what
        QStandardPaths returns for GenericDataLocation. If that doesn't exist, then
        we are not running on a freedesktop.org-compliant environment, and give up.
    */
    if (trash.isEmpty()) {
        QDir topDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        trash = makeTrashDir(topDir, QLatin1String("Trash"));
        if (!QFileInfo(trash).isDir()) {
            qWarning("Unable to establish trash directory in %s",
                     topDir.path().toLocal8Bit().constData());
        }
    }

    return trash;
}
#endif // QT_BOOTSTRAPPED

//static
bool QFileSystemEngine::moveFileToTrash(const QFileSystemEntry &source,
                                        QFileSystemEntry &newLocation, QSystemError &error)
{
#ifdef QT_BOOTSTRAPPED
    Q_UNUSED(source);
    Q_UNUSED(newLocation);
    error = QSystemError(ENOSYS, QSystemError::StandardLibraryError);
    return false;
#else
    const QFileInfo sourceInfo(source.filePath());
    if (!sourceInfo.exists()) {
        error = QSystemError(ENOENT, QSystemError::StandardLibraryError);
        return false;
    }
    const QString sourcePath = sourceInfo.absoluteFilePath();

    QDir trashDir(freeDesktopTrashLocation(sourcePath));
    if (!trashDir.exists())
        return false;
    /*
        "A trash directory contains two subdirectories, named info and files."
    */
    const QLatin1String filesDir("files");
    const QLatin1String infoDir("info");
    trashDir.mkdir(filesDir);
    int savedErrno = errno;
    trashDir.mkdir(infoDir);
    if (!savedErrno)
        savedErrno = errno;
    if (!trashDir.exists(filesDir) || !trashDir.exists(infoDir)) {
        error = QSystemError(savedErrno, QSystemError::StandardLibraryError);
        return false;
    }
    /*
        "The $trash/files directory contains the files and directories that were trashed.
         The names of files in this directory are to be determined by the implementation;
         the only limitation is that they must be unique within the directory. Even if a
         file with the same name and location gets trashed many times, each subsequent
         trashing must not overwrite a previous copy."
    */
    const QString trashedName = sourceInfo.isDir()
                              ? QDir(sourcePath).dirName()
                              : sourceInfo.fileName();
    QString uniqueTrashedName = QLatin1Char('/') + trashedName;
    QString infoFileName;
    int counter = 0;
    QFile infoFile;
    auto makeUniqueTrashedName = [trashedName, &counter]() -> QString {
        ++counter;
        return QString(QLatin1String("/%1-%2"))
                                        .arg(trashedName)
                                        .arg(counter, 4, 10, QLatin1Char('0'));
    };
    do {
        while (QFile::exists(trashDir.filePath(filesDir) + uniqueTrashedName))
            uniqueTrashedName = makeUniqueTrashedName();
        /*
            "The $trash/info directory contains an "information file" for every file and directory
             in $trash/files. This file MUST have exactly the same name as the file or directory in
             $trash/files, plus the extension ".trashinfo"
             [...]
             When trashing a file or directory, the implementation MUST create the corresponding
             file in $trash/info first. Moreover, it MUST try to do this in an atomic fashion,
             so that if two processes try to trash files with the same filename this will result
             in two different trash files. On Unix-like systems this is done by generating a
             filename, and then opening with O_EXCL. If that succeeds the creation was atomic
             (at least on the same machine), if it fails you need to pick another filename."
        */
        infoFileName = trashDir.filePath(infoDir)
                     + uniqueTrashedName + QLatin1String(".trashinfo");
        infoFile.setFileName(infoFileName);
        if (!infoFile.open(QIODevice::NewOnly | QIODevice::WriteOnly | QIODevice::Text))
            uniqueTrashedName = makeUniqueTrashedName();
    } while (!infoFile.isOpen());

    const QString targetPath = trashDir.filePath(filesDir) + uniqueTrashedName;
    const QFileSystemEntry target(targetPath);

    /*
        We might fail to rename if source and target are on different file systems.
        In that case, we don't try further, i.e. copying and removing the original
        is usually not what the user would expect to happen.
    */
    if (!renameFile(source, target, error)) {
        infoFile.close();
        infoFile.remove();
        return false;
    }

    QByteArray info =
            "[Trash Info]\n"
            "Path=" + sourcePath.toUtf8() + "\n"
            "DeletionDate=" + QDateTime::currentDateTime().toString(QLatin1String("yyyy-MM-ddThh:mm:ss")).toUtf8()
            + "\n";
    infoFile.write(info);
    infoFile.close();

    newLocation = QFileSystemEntry(targetPath);
    return true;
#endif // QT_BOOTSTRAPPED
}
#endif // Q_OS_DARWIN

//static
bool QFileSystemEngine::copyFile(const QFileSystemEntry &source, const QFileSystemEntry &target, QSystemError &error)
{
#if defined(Q_OS_DARWIN)
    if (::clonefile(source.nativeFilePath().constData(),
                    target.nativeFilePath().constData(), 0) == 0)
        return true;
    error = QSystemError(errno, QSystemError::StandardLibraryError);
    return false;
#else
    Q_UNUSED(source);
    Q_UNUSED(target);
    error = QSystemError(ENOSYS, QSystemError::StandardLibraryError); //Function not implemented
    return false;
#endif
}

//static
bool QFileSystemEngine::renameFile(const QFileSystemEntry &source, const QFileSystemEntry &target, QSystemError &error)
{
    QFileSystemEntry::NativePath srcPath = source.nativeFilePath();
    QFileSystemEntry::NativePath tgtPath = target.nativeFilePath();

    Q_CHECK_FILE_NAME(srcPath, false);
    Q_CHECK_FILE_NAME(tgtPath, false);

#if defined(RENAME_NOREPLACE) && QT_CONFIG(renameat2)
    if (renameat2(AT_FDCWD, srcPath, AT_FDCWD, tgtPath, RENAME_NOREPLACE) == 0)
        return true;

    // We can also get EINVAL for some non-local filesystems.
    if (errno != EINVAL) {
        error = QSystemError(errno, QSystemError::StandardLibraryError);
        return false;
    }
#endif
#if defined(Q_OS_DARWIN) && defined(RENAME_EXCL)
    if (renameatx_np(AT_FDCWD, srcPath, AT_FDCWD, tgtPath, RENAME_EXCL) == 0)
        return true;
    if (errno != ENOTSUP) {
        error = QSystemError(errno, QSystemError::StandardLibraryError);
        return false;
    }
#endif

    if (SupportsHardlinking && ::link(srcPath, tgtPath) == 0) {
        if (::unlink(srcPath) == 0)
            return true;

        // if we managed to link but can't unlink the source, it's likely
        // it's in a directory we don't have write access to; fail the
        // renaming instead
        int savedErrno = errno;

        // this could fail too, but there's nothing we can do about it now
        ::unlink(tgtPath);

        error = QSystemError(savedErrno, QSystemError::StandardLibraryError);
        return false;
    } else if (!SupportsHardlinking) {
        // man 2 link on Linux has:
        // EPERM  The filesystem containing oldpath and newpath does not
        //        support the creation of hard links.
        errno = EPERM;
    }

    switch (errno) {
    case EACCES:
    case EEXIST:
    case ENAMETOOLONG:
    case ENOENT:
    case ENOTDIR:
    case EROFS:
    case EXDEV:
        // accept the error from link(2) (especially EEXIST) and don't retry
        break;

    default:
        // fall back to rename()
        // ### Race condition. If a file is moved in after this, it /will/ be
        // overwritten.
        if (::rename(srcPath, tgtPath) == 0)
            return true;
    }

    error = QSystemError(errno, QSystemError::StandardLibraryError);
    return false;
}

//static
bool QFileSystemEngine::renameOverwriteFile(const QFileSystemEntry &source, const QFileSystemEntry &target, QSystemError &error)
{
    Q_CHECK_FILE_NAME(source, false);
    Q_CHECK_FILE_NAME(target, false);

    if (::rename(source.nativeFilePath().constData(), target.nativeFilePath().constData()) == 0)
        return true;
    error = QSystemError(errno, QSystemError::StandardLibraryError);
    return false;
}

//static
bool QFileSystemEngine::removeFile(const QFileSystemEntry &entry, QSystemError &error)
{
    Q_CHECK_FILE_NAME(entry, false);
    if (unlink(entry.nativeFilePath().constData()) == 0)
        return true;
    error = QSystemError(errno, QSystemError::StandardLibraryError);
    return false;

}

//static
bool QFileSystemEngine::setPermissions(const QFileSystemEntry &entry, QFile::Permissions permissions, QSystemError &error, QFileSystemMetaData *data)
{
    Q_CHECK_FILE_NAME(entry, false);

    mode_t mode = QtPrivate::toMode_t(permissions);
    bool success = ::chmod(entry.nativeFilePath().constData(), mode) == 0;
    if (success && data) {
        data->entryFlags &= ~QFileSystemMetaData::Permissions;
        data->entryFlags |= QFileSystemMetaData::MetaDataFlag(uint(permissions.toInt()));
        data->knownFlagsMask |= QFileSystemMetaData::Permissions;
    }
    if (!success)
        error = QSystemError(errno, QSystemError::StandardLibraryError);
    return success;
}

//static
bool QFileSystemEngine::setPermissions(int fd, QFile::Permissions permissions, QSystemError &error, QFileSystemMetaData *data)
{
    mode_t mode = QtPrivate::toMode_t(permissions);

    bool success = ::fchmod(fd, mode) == 0;
    if (success && data) {
        data->entryFlags &= ~QFileSystemMetaData::Permissions;
        data->entryFlags |= QFileSystemMetaData::MetaDataFlag(uint(permissions.toInt()));
        data->knownFlagsMask |= QFileSystemMetaData::Permissions;
    }
    if (!success)
        error = QSystemError(errno, QSystemError::StandardLibraryError);
    return success;
}

//static
bool QFileSystemEngine::setFileTime(int fd, const QDateTime &newDate, QAbstractFileEngine::FileTime time, QSystemError &error)
{
    if (!newDate.isValid() || time == QAbstractFileEngine::BirthTime ||
            time == QAbstractFileEngine::MetadataChangeTime) {
        error = QSystemError(EINVAL, QSystemError::StandardLibraryError);
        return false;
    }

#if QT_CONFIG(futimens)
    struct timespec ts[2];

    ts[0].tv_sec = ts[1].tv_sec = 0;
    ts[0].tv_nsec = ts[1].tv_nsec = UTIME_OMIT;

    const qint64 msecs = newDate.toMSecsSinceEpoch();

    if (time == QAbstractFileEngine::AccessTime) {
        ts[0].tv_sec = msecs / 1000;
        ts[0].tv_nsec = (msecs % 1000) * 1000000;
    } else if (time == QAbstractFileEngine::ModificationTime) {
        ts[1].tv_sec = msecs / 1000;
        ts[1].tv_nsec = (msecs % 1000) * 1000000;
    }

    if (futimens(fd, ts) == -1) {
        error = QSystemError(errno, QSystemError::StandardLibraryError);
        return false;
    }

    return true;
#elif QT_CONFIG(futimes)
    struct timeval tv[2];
    QT_STATBUF st;

    if (QT_FSTAT(fd, &st) == -1) {
        error = QSystemError(errno, QSystemError::StandardLibraryError);
        return false;
    }

    GetFileTimes::get(&st, &tv[0], &tv[1]);

    const qint64 msecs = newDate.toMSecsSinceEpoch();

    if (time == QAbstractFileEngine::AccessTime) {
        tv[0].tv_sec = msecs / 1000;
        tv[0].tv_usec = (msecs % 1000) * 1000;
    } else if (time == QAbstractFileEngine::ModificationTime) {
        tv[1].tv_sec = msecs / 1000;
        tv[1].tv_usec = (msecs % 1000) * 1000;
    }

    if (futimes(fd, tv) == -1) {
        error = QSystemError(errno, QSystemError::StandardLibraryError);
        return false;
    }

    return true;
#else
    Q_UNUSED(fd);
    error = QSystemError(ENOSYS, QSystemError::StandardLibraryError);
    return false;
#endif
}

QString QFileSystemEngine::homePath()
{
    QString home = QFile::decodeName(qgetenv("HOME"));
    if (home.isEmpty())
        home = rootPath();
    return QDir::cleanPath(home);
}

QString QFileSystemEngine::rootPath()
{
    return QLatin1String("/");
}

QString QFileSystemEngine::tempPath()
{
#ifdef QT_UNIX_TEMP_PATH_OVERRIDE
    return QLatin1String(QT_UNIX_TEMP_PATH_OVERRIDE);
#else
    QString temp = QFile::decodeName(qgetenv("TMPDIR"));
    if (temp.isEmpty()) {
        if (false) {
#if defined(Q_OS_DARWIN) && !defined(QT_BOOTSTRAPPED)
        } else if (NSString *nsPath = NSTemporaryDirectory()) {
            temp = QString::fromCFString((CFStringRef)nsPath);
#endif
        } else {
            temp = QLatin1String(_PATH_TMP);
        }
    }
    return QDir(QDir::cleanPath(temp)).canonicalPath();
#endif
}

bool QFileSystemEngine::setCurrentPath(const QFileSystemEntry &path)
{
    int r;
    r = QT_CHDIR(path.nativeFilePath().constData());
    return r >= 0;
}

QFileSystemEntry QFileSystemEngine::currentPath()
{
    QFileSystemEntry result;
#if defined(__GLIBC__) && !defined(PATH_MAX)
    char *currentName = ::get_current_dir_name();
    if (currentName) {
        result = QFileSystemEntry(QByteArray(currentName), QFileSystemEntry::FromNativePath());
        ::free(currentName);
    }
#else
    char currentName[PATH_MAX+1];
    if (::getcwd(currentName, PATH_MAX)) {
#if defined(Q_OS_VXWORKS) && defined(VXWORKS_VXSIM)
        QByteArray dir(currentName);
        if (dir.indexOf(':') < dir.indexOf('/'))
            dir.remove(0, dir.indexOf(':')+1);

        qstrncpy(currentName, dir.constData(), PATH_MAX);
#endif
        result = QFileSystemEntry(QByteArray(currentName), QFileSystemEntry::FromNativePath());
    }
# if defined(QT_DEBUG)
    if (result.isEmpty())
        qWarning("QFileSystemEngine::currentPath: getcwd() failed");
# endif
#endif
    return result;
}
QT_END_NAMESPACE
