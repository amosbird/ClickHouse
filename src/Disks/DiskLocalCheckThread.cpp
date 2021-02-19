#include <Disks/DiskLocalCheckThread.h>

#include <Disks/DiskLocal.h>
#include <Interpreters/Context.h>

namespace DB
{
static const auto DISK_CHECK_ERROR_SLEEP_MS = 1000;
static const auto DISK_CHECK_ERROR_RETRY_TIME = 3;

DiskLocalCheckThread::DiskLocalCheckThread(DiskLocal * disk_, ContextConstPtr context, UInt64 local_disk_check_period_ms)
    : disk(std::move(disk_))
    , check_period_ms(local_disk_check_period_ms)
    , log_name(disk->getName() + "@" + disk->getPath() + " (DiskLocalCheckThread)")
    , log(&Poco::Logger::get(log_name))
{
    task = context->getSchedulePool().createTask(log_name, [this] { run(); });
}

void DiskLocalCheckThread::start()
{
    task->activateAndSchedule();
}

void DiskLocalCheckThread::wakeup()
{
    task->schedule();
}

void DiskLocalCheckThread::run()
{
    if (need_stop)
        return;

    if (disk->setupAndCheck())
    {
        if (disk->broken)
        {
            if (disk->shouldRecover())
            {
                disk->broken = false;
                LOG_INFO(log, "Disk is recovered.");
            }
            else
            {
                LOG_INFO(log, "Disk seems to be fine. It can be recovered by creating an .recover file in {}.", disk->getPath());
            }
        }
        retry = 0;
        task->scheduleAfter(check_period_ms);
    }
    else if (!disk->broken && retry < DISK_CHECK_ERROR_RETRY_TIME)
    {
        ++retry;
        task->scheduleAfter(DISK_CHECK_ERROR_SLEEP_MS);
    }
    else
    {
        retry = 0;
        disk->broken = true;
        LOG_INFO(log, "Disk is broken.");
        task->scheduleAfter(check_period_ms);
    }
}

void DiskLocalCheckThread::shutdown()
{
    need_stop = true;
    task->deactivate();
    LOG_TRACE(log, "DiskLocalCheck thread finished");
}

}
