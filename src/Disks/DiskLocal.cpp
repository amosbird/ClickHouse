#include <Disks/DiskLocal.h>

#include <Disks/DiskFactory.h>
#include <Disks/DiskMemory.h>
#include <Disks/LocalDirectorySyncGuard.h>
#include <IO/createReadBufferFromFileBase.h>
#include <Interpreters/Context.h>
#include <Common/createHardLink.h>
#include <Common/filesystemHelpers.h>
#include <Common/quoteString.h>
#include <Common/randomSeed.h>

#include <unistd.h>
#include <sys/stat.h>
#include <common/logger_useful.h>


namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int UNKNOWN_ELEMENT_IN_CONFIG;
    extern const int EXCESSIVE_ELEMENT_IN_CONFIG;
    extern const int INCORRECT_DISK_INDEX;
    extern const int CANNOT_TRUNCATE_FILE;
    extern const int CANNOT_UNLINK;
    extern const int CANNOT_RMDIR;
}

std::mutex DiskLocal::reservation_mutex;


using DiskLocalPtr = std::shared_ptr<DiskLocal>;

class DiskLocalReservation : public IReservation
{
public:
    DiskLocalReservation(const DiskLocalPtr & disk_, UInt64 size_)
        : disk(disk_), size(size_), metric_increment(CurrentMetrics::DiskSpaceReservedForMerge, size_)
    {
    }

    UInt64 getSize() const override { return size; }

    DiskPtr getDisk(size_t i) const override
    {
        if (i != 0)
        {
            throw Exception("Can't use i != 0 with single disk reservation", ErrorCodes::INCORRECT_DISK_INDEX);
        }
        return disk;
    }

    Disks getDisks() const override { return {disk}; }

    void update(UInt64 new_size) override
    {
        std::lock_guard lock(DiskLocal::reservation_mutex);
        disk->reserved_bytes -= size;
        size = new_size;
        disk->reserved_bytes += size;
    }

    ~DiskLocalReservation() override
    {
        try
        {
            std::lock_guard lock(DiskLocal::reservation_mutex);
            if (disk->reserved_bytes < size)
            {
                disk->reserved_bytes = 0;
                LOG_ERROR(&Poco::Logger::get("DiskLocal"), "Unbalanced reservations size for disk '{}'.", disk->getName());
            }
            else
            {
                disk->reserved_bytes -= size;
            }

            if (disk->reservation_count == 0)
                LOG_ERROR(&Poco::Logger::get("DiskLocal"), "Unbalanced reservation count for disk '{}'.", disk->getName());
            else
                --disk->reservation_count;
        }
        catch (...)
        {
            tryLogCurrentException(__PRETTY_FUNCTION__);
        }
    }

private:
    DiskLocalPtr disk;
    UInt64 size;
    CurrentMetrics::Increment metric_increment;
};


class DiskLocalDirectoryIterator : public IDiskDirectoryIterator
{
public:
    explicit DiskLocalDirectoryIterator(const String & disk_path_, const String & dir_path_)
        : dir_path(dir_path_), iter(disk_path_ + dir_path_)
    {
    }

    void next() override { ++iter; }

    bool isValid() const override { return iter != Poco::DirectoryIterator(); }

    String path() const override
    {
        if (iter->isDirectory())
            return dir_path + iter.name() + '/';
        else
            return dir_path + iter.name();
    }

    String name() const override { return iter.name(); }

private:
    String dir_path;
    Poco::DirectoryIterator iter;
};


ReservationPtr DiskLocal::reserve(UInt64 bytes)
{
    if (!tryReserve(bytes))
        return {};
    return std::make_unique<DiskLocalReservation>(std::static_pointer_cast<DiskLocal>(shared_from_this()), bytes);
}

bool DiskLocal::tryReserve(UInt64 bytes)
{
    std::lock_guard lock(DiskLocal::reservation_mutex);
    if (bytes == 0)
    {
        LOG_DEBUG(log, "Reserving 0 bytes on disk {}", backQuote(name));
        ++reservation_count;
        return true;
    }

    auto available_space = getAvailableSpace();
    UInt64 unreserved_space = available_space - std::min(available_space, reserved_bytes);
    if (unreserved_space >= bytes)
    {
        LOG_DEBUG(log, "Reserving {} on disk {}, having unreserved {}.",
            ReadableSize(bytes), backQuote(name), ReadableSize(unreserved_space));
        ++reservation_count;
        reserved_bytes += bytes;
        return true;
    }
    return false;
}

static UInt64 getTotalSpaceByName(const String & name, const String & disk_path, UInt64 keep_free_space_bytes)
{
    struct statvfs fs;
    if (name == "default") /// for default disk we get space from path/data/
        fs = getStatVFS(disk_path + "data/");
    else
        fs = getStatVFS(disk_path);
    UInt64 total_size = fs.f_blocks * fs.f_bsize;
    if (total_size < keep_free_space_bytes)
        return 0;
    return total_size - keep_free_space_bytes;
}

UInt64 DiskLocal::getTotalSpace() const
{
    if (broken)
        return 0;
    return getTotalSpaceByName(name, disk_path, keep_free_space_bytes);
}

UInt64 DiskLocal::getAvailableSpace() const
{
    if (broken)
        return 0;
    /// we use f_bavail, because part of b_free space is
    /// available for superuser only and for system purposes
    struct statvfs fs;
    if (name == "default") /// for default disk we get space from path/data/
        fs = getStatVFS(disk_path + "data/");
    else
        fs = getStatVFS(disk_path);
    UInt64 total_size = fs.f_bavail * fs.f_bsize;
    if (total_size < keep_free_space_bytes)
        return 0;
    return total_size - keep_free_space_bytes;
}

UInt64 DiskLocal::getUnreservedSpace() const
{
    std::lock_guard lock(DiskLocal::reservation_mutex);
    auto available_space = getAvailableSpace();
    available_space -= std::min(available_space, reserved_bytes);
    return available_space;
}

bool DiskLocal::exists(const String & path) const
{
    return Poco::File(disk_path + path).exists();
}

bool DiskLocal::isFile(const String & path) const
{
    return Poco::File(disk_path + path).isFile();
}

bool DiskLocal::isDirectory(const String & path) const
{
    return Poco::File(disk_path + path).isDirectory();
}

size_t DiskLocal::getFileSize(const String & path) const
{
    return Poco::File(disk_path + path).getSize();
}

void DiskLocal::createDirectory(const String & path)
{
    Poco::File(disk_path + path).createDirectory();
}

void DiskLocal::createDirectories(const String & path)
{
    Poco::File(disk_path + path).createDirectories();
}

void DiskLocal::clearDirectory(const String & path)
{
    std::vector<Poco::File> files;
    Poco::File(disk_path + path).list(files);
    for (auto & file : files)
        file.remove();
}

void DiskLocal::moveDirectory(const String & from_path, const String & to_path)
{
    Poco::File(disk_path + from_path).renameTo(disk_path + to_path);
}

DiskDirectoryIteratorPtr DiskLocal::iterateDirectory(const String & path)
{
    if (broken)
        return std::make_unique<DiskMemoryDirectoryIterator>(std::vector<Poco::Path>{});
    return std::make_unique<DiskLocalDirectoryIterator>(disk_path, path);
}

void DiskLocal::moveFile(const String & from_path, const String & to_path)
{
    Poco::File(disk_path + from_path).renameTo(disk_path + to_path);
}

void DiskLocal::replaceFile(const String & from_path, const String & to_path)
{
    Poco::File from_file(disk_path + from_path);
    Poco::File to_file(disk_path + to_path);
    if (to_file.exists())
    {
        Poco::File tmp_file(disk_path + to_path + ".old");
        to_file.renameTo(tmp_file.path());
        from_file.renameTo(disk_path + to_path);
        tmp_file.remove();
    }
    else
        from_file.renameTo(to_file.path());
}

std::unique_ptr<ReadBufferFromFileBase>
DiskLocal::readFile(
    const String & path, size_t buf_size, size_t estimated_size, size_t aio_threshold, size_t mmap_threshold, MMappedFileCache * mmap_cache) const
{
    return createReadBufferFromFileBase(disk_path + path, estimated_size, aio_threshold, mmap_threshold, mmap_cache, buf_size);
}

std::unique_ptr<WriteBufferFromFileBase> DiskLocal::writeFile(const String & path, size_t buf_size, WriteMode mode)
{
    int flags = (mode == WriteMode::Append) ? (O_APPEND | O_CREAT | O_WRONLY) : -1;
    return std::make_unique<WriteBufferFromFile>(disk_path + path, buf_size, flags);
}

void DiskLocal::removeFile(const String & path)
{
    auto fs_path = disk_path + path;
    if (0 != unlink(fs_path.c_str()))
        throwFromErrnoWithPath("Cannot unlink file " + fs_path, fs_path, ErrorCodes::CANNOT_UNLINK);
}

void DiskLocal::removeFileIfExists(const String & path)
{
    auto fs_path = disk_path + path;
    if (0 != unlink(fs_path.c_str()) && errno != ENOENT)
        throwFromErrnoWithPath("Cannot unlink file " + fs_path, fs_path, ErrorCodes::CANNOT_UNLINK);
}

void DiskLocal::removeDirectory(const String & path)
{
    auto fs_path = disk_path + path;
    if (0 != rmdir(fs_path.c_str()))
        throwFromErrnoWithPath("Cannot rmdir " + fs_path, fs_path, ErrorCodes::CANNOT_RMDIR);
}

void DiskLocal::removeRecursive(const String & path)
{
    Poco::File(disk_path + path).remove(true);
}

void DiskLocal::listFiles(const String & path, std::vector<String> & file_names)
{
    Poco::File(disk_path + path).list(file_names);
}

void DiskLocal::setLastModified(const String & path, const Poco::Timestamp & timestamp)
{
    Poco::File(disk_path + path).setLastModified(timestamp);
}

Poco::Timestamp DiskLocal::getLastModified(const String & path)
{
    return Poco::File(disk_path + path).getLastModified();
}

void DiskLocal::createHardLink(const String & src_path, const String & dst_path)
{
    DB::createHardLink(disk_path + src_path, disk_path + dst_path);
}

void DiskLocal::truncateFile(const String & path, size_t size)
{
    int res = truncate((disk_path + path).c_str(), size);
    if (-1 == res)
        throwFromErrnoWithPath("Cannot truncate file " + path, path, ErrorCodes::CANNOT_TRUNCATE_FILE);
}

void DiskLocal::createFile(const String & path)
{
    Poco::File(disk_path + path).createFile();
}

void DiskLocal::setReadOnly(const String & path)
{
    Poco::File(disk_path + path).setReadOnly(true);
}

bool inline isSameDiskType(const IDisk & one, const IDisk & another)
{
    return typeid(one) == typeid(another);
}

void DiskLocal::copy(const String & from_path, const std::shared_ptr<IDisk> & to_disk, const String & to_path)
{
    if (isSameDiskType(*this, *to_disk))
        Poco::File(disk_path + from_path).copyTo(to_disk->getPath() + to_path); /// Use more optimal way.
    else
        IDisk::copy(from_path, to_disk, to_path); /// Copy files through buffers.
}

SyncGuardPtr DiskLocal::getDirectorySyncGuard(const String & path) const
{
    return std::make_unique<LocalDirectorySyncGuard>(disk_path + path);
}

bool DiskLocal::setupAndCheck()
{
    Poco::File disk{disk_path};
    try
    {
        disk.createDirectories();
        if (!disk.canRead() || !disk.canWrite())
        {
            LOG_WARNING(logger, "There is no RW access to disk {} ({}).", name, disk_path);
            return false;
        }
    }
    catch (...)
    {
        LOG_WARNING(logger, "Cannot create the directory of disk {} ({}).", name, disk_path);
        return false;
    }
    String file_path = disk_path + ".check";
    const char * fpath = file_path.c_str();
    char errmsg[64];
    if (::access(fpath, F_OK) == 0)
    {
        if (::remove(fpath) != 0)
        {
            LOG_WARNING(logger, "Fail to delete test file. path = {}, errno = {}, err = {}.", fpath, errno, strerror_r(errno, errmsg, 64));
            return false;
        }
    }
    else
    {
        if (errno != ENOENT)
        {
            LOG_WARNING(logger, "Fail to access test file. path = {}, errno = {}, err = {}.", fpath, errno, strerror_r(errno, errmsg, 64));
            return false;
        }
    }
    int fd;
    if ((fd = ::open(fpath, O_RDWR | O_CREAT | O_SYNC, S_IRUSR | S_IWUSR)) < 0)
    {
        LOG_WARNING(logger, "Fail to create test file. path={}.", fpath);
        return false;
    }

    bool read_write_ok = true;
    if (disk_checker)
    {
        auto rw_test = [fd](char * buf, auto && func)
        {
            char * ptr = buf;
            auto buf_size = DiskLocalCheckThread::BUF_SIZE;
            size_t offset = 0;
            while (buf_size > 0)
            {
                ssize_t rd_size = func(fd, ptr, buf_size, offset);
                if (rd_size <= 0)
                    return false;
                buf_size -= rd_size;
                ptr += rd_size;
                offset += rd_size;
            }
            return true;
        };

        if (!rw_test(disk_checker->wbuf, ::pwrite))
        {
            LOG_WARNING(logger, "Fail to write test file. [file_name={}, err={}].", fpath, strerror_r(errno, errmsg, 64));
            read_write_ok = false;
        }
        else if (!rw_test(disk_checker->rbuf, ::pread))
        {
            LOG_WARNING(logger, "Fail to read test file. [file_name={}, err={}].", fpath, strerror_r(errno, errmsg, 64));
            read_write_ok = false;
        }
        else if (memcmp(disk_checker->wbuf, disk_checker->rbuf, DiskLocalCheckThread::BUF_SIZE) != 0)
        {
            LOG_WARNING(logger, "Inconsistent read and write over test file, [file_name = {}].", fpath);
            read_write_ok = false;
        }
    }
    if (::close(fd) != 0)
    {
        LOG_WARNING(logger, "Fail to close test file. [file_name={}].", fpath);
        return false;
    }
    if (::remove(fpath) != 0)
    {
        LOG_WARNING(logger, "Fail to delete test file. [err={}, path={}].", strerror_r(errno, errmsg, 64), fpath);
        return false;
    }
    return read_write_ok;
}

bool DiskLocal::shouldRecover() const
{
    Poco::File disk{disk_path};
    String file_path = disk_path + ".recover";
    const char * fpath = file_path.c_str();
    char errmsg[64];
    if (::access(fpath, F_OK) == 0)
    {
        if (::remove(fpath) != 0)
        {
            LOG_WARNING(logger, "Fail to delete test file. path = {}, errno = {}, err = {}.", fpath, errno, strerror_r(errno, errmsg, 64));
            return false;
        }
        return true;
    }
    return false;
}

DiskLocal::DiskLocal(const String & name_, const String & path_, UInt64 keep_free_space_bytes_)
    : name(name_), disk_path(path_), keep_free_space_bytes(keep_free_space_bytes_), logger(&Poco::Logger::get("DiskLocal"))
{
    if (disk_path.back() != '/')
        throw Exception("Disk path must ends with '/', but '" + disk_path + "' doesn't.", ErrorCodes::LOGICAL_ERROR);
    broken = !setupAndCheck();
}

DiskLocal::DiskLocal(
    const String & name_, const String & path_, UInt64 keep_free_space_bytes_, ContextConstPtr context, UInt64 local_disk_check_period_ms)
    : DiskLocal(name_, path_, keep_free_space_bytes_)
{
    if (local_disk_check_period_ms > 0)
    {
        disk_checker = std::make_unique<DiskLocalCheckThread>(this, context, local_disk_check_period_ms);
        pcg64_fast rng(randomSeed());
        std::generate_n(disk_checker->wbuf, DiskLocalCheckThread::BUF_SIZE, std::ref(rng));
        disk_checker->start();
    }
}

void registerDiskLocal(DiskFactory & factory)
{
    auto creator = [](const String & name,
                      const Poco::Util::AbstractConfiguration & config,
                      const String & config_prefix,
                      ContextConstPtr context) -> DiskPtr
    {
        String path = config.getString(config_prefix + ".path", "");
        if (name == "default")
        {
            if (!path.empty())
                throw Exception(
                    "\"default\" disk path should be provided in <path> not in <storage_configuration>",
                    ErrorCodes::UNKNOWN_ELEMENT_IN_CONFIG);
            path = context->getPath();
        }
        else
        {
            if (path.empty())
                throw Exception("Disk path can not be empty. Disk " + name, ErrorCodes::UNKNOWN_ELEMENT_IN_CONFIG);
            if (path.back() != '/')
                path += "/";
        }

        bool has_space_ratio = config.has(config_prefix + ".keep_free_space_ratio");

        if (config.has(config_prefix + ".keep_free_space_bytes") && has_space_ratio)
            throw Exception(
                "Only one of 'keep_free_space_bytes' and 'keep_free_space_ratio' can be specified",
                ErrorCodes::EXCESSIVE_ELEMENT_IN_CONFIG);

        UInt64 keep_free_space_bytes = config.getUInt64(config_prefix + ".keep_free_space_bytes", 0);

        if (has_space_ratio)
        {
            auto ratio = config.getDouble(config_prefix + ".keep_free_space_ratio");
            if (ratio < 0 || ratio > 1)
                throw Exception("'keep_free_space_ratio' have to be between 0 and 1", ErrorCodes::EXCESSIVE_ELEMENT_IN_CONFIG);
            String tmp_path = path;
            if (tmp_path.empty())
                tmp_path = context->getPath();

            // Create tmp disk for getting total disk space.
            keep_free_space_bytes = static_cast<UInt64>(getTotalSpaceByName("tmp", tmp_path, 0) * ratio);
        }

        return std::make_shared<DiskLocal>(name, path, keep_free_space_bytes, context, config.getUInt("local_disk_check_period_ms", 0));
    };
    factory.registerDiskType("local", creator);
}

}
