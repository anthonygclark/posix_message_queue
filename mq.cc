#include <cassert>
#include <cerrno>
#include <climits>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#if defined(MQ_LOG_STDERR)
#include <cstdio>
#endif

extern "C"
{
#include <mqueue.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
}

#include "mq.hh"

/* Always use sysconf for now. */
#define MQ_USE_SYSCONF

namespace ipc
{
    inline namespace v1
    {
        namespace detail
        {
            /**< Init flags for the follow values. We really only want it to happen once. */
            std::once_flag mq_proc_sys_fs_init;

            unsigned long mq_proc_sys_fs_mqueue_msg_default = 0;
            unsigned long mq_proc_sys_fs_mqueue_msg_max = 0;
            unsigned long mq_proc_sys_fs_mqueue_msgsize_default = 0;
            unsigned long mq_proc_sys_fs_mqueue_msgsize_max = 0;
            unsigned long mq_proc_sys_fs_mqueue_queues_max = 0;
            unsigned long mq_max_priority = 0;
            unsigned long mq_min_priority = 0;

            /**
             * @brief Initializes namespace-level variables for message queue limits.
             */
            void mq_init_proc_sys_fs_values()
            {
                /* NOTE: 31 is found in mq_open(3) */
#ifdef MQ_USE_SYSCONF
                auto _mp = ::sysconf(_SC_MQ_PRIO_MAX);

                /* default if sysconf fails */
                mq_max_priority = _mp == -1 ? 31 : _mp - 1;
#else
                mq_max_priority = 31;
#endif

                /* There is a no ifdef around procfs use since posix MQs really only work on linux
                 * and the proc fs interface has existed almost the entire time MQs have been allowed...
                 */
                mq_proc_sys_fs_mqueue_msg_default = detail::read_procfs_file<unsigned long>("/proc/sys/fs/mqueue/msg_default\0");
                mq_proc_sys_fs_mqueue_msg_max = detail::read_procfs_file<unsigned long>("/proc/sys/fs/mqueue/msg_max\0");
                mq_proc_sys_fs_mqueue_msgsize_default = detail::read_procfs_file<unsigned long>("/proc/sys/fs/mqueue/msgsize_default\0");
                mq_proc_sys_fs_mqueue_msgsize_max = detail::read_procfs_file<unsigned long>("/proc/sys/fs/mqueue/msgsize_max\0");
                mq_proc_sys_fs_mqueue_queues_max = detail::read_procfs_file<unsigned long>("/proc/sys/fs/mqueue/queues_max\0");
            }

        } /* end namespace detail */

        /*** message_queue implementation ***/

        void message_queue::flush()
        {
            assert(m_mq_descriptor != -1 && "MQ must be opened first");

            /* double check... */
            assert(flushable() && "Must be flushable");

            auto buf = new message_queue::buffer_type[recv_buffer_size()];

#if defined(MQ_LOG_STDERR)
            {
                auto curmsgs = get_message_count();
                std::fprintf(stderr, "Flushing %ld messages from %s\n", curmsgs, m_name.c_str());
            }
#endif
            /* Is this too slow? */
            while (auto curmsgs = get_message_count() > 0)
                recv(buf);

            delete[] buf;
        }

        bool message_queue::name_is_valid(std::string const & name)
        {
            if (name.length() > NAME_MAX) return false; /* too long */
            if (name.length() < 2) return false; /* empty or only slash */
            if (name.length() && *name.cbegin() != '/') return false; /* no slash */
            if (name.find_last_of('/') != 0) return false; /* more than one slash */
            return true;
        }

        message_queue::message_queue(std::string const & name, unsigned int flags,
                                     mode_t perms, long max_messages, long max_message_size) :
            m_flags(flags),
            m_name(name),
            m_mq_descriptor(-1)
        {
            /* initialize defaults */
            std::call_once(detail::mq_proc_sys_fs_init, detail::mq_init_proc_sys_fs_values);

            /* check for actual flags */
            if (!m_flags)
                throw std::runtime_error("flags must contain a value");

            std::memset(&m_attr_cache, 0, sizeof(m_attr_cache));

            /* try to use max_messages or max_message_size on the new queue */
            if ((max_messages != -1) || (max_message_size != -1))
            {
                mq_attr new_attrs = {0};

                if (max_messages != -1) {
                    if (max_messages > detail::mq_proc_sys_fs_mqueue_msg_max)
                        throw make_syserr(std::errc::invalid_argument, "Too many messages for system.");

                    new_attrs.mq_maxmsg = max_messages;
                }
                else {
                    /* default */
                    new_attrs.mq_maxmsg = detail::mq_proc_sys_fs_mqueue_msg_max;
                }

                if (max_message_size != -1) {
                    if (max_message_size > detail::mq_proc_sys_fs_mqueue_msgsize_max)
                        throw make_syserr(std::errc::invalid_argument, "Max message exceeded for system.");

                    new_attrs.mq_msgsize = max_message_size;
                }
                else {
                    /* default */
                    new_attrs.mq_msgsize = detail::mq_proc_sys_fs_mqueue_msgsize_max;
                }

                /* You can only set MQ_NONBLOCK in new attributes as per the man page */
                new_attrs.mq_flags = ((m_flags & MQ_NONBLOCK) == MQ_NONBLOCK) ? MQ_NONBLOCK : 0;

                /* open the queue with new attrs.
                 * Note that mq_open won't let you attach to a queue that's already open
                 * to change its attributes. So the mismatch errors are to prevent you
                 * from being mislead that you changed the attributes
                 */
                m_mq_descriptor = ::mq_open(m_name.c_str(), m_flags, perms, &new_attrs);

                if (m_mq_descriptor == -1)
                    throw make_syserr(errno, "ctor::mq_open");

                /* Note that this saves the attrs to m_attr_cache */
                if (::mq_getattr(m_mq_descriptor, &m_attr_cache) == -1)
                    throw make_syserr(errno, "::mq_getattr");

                if (new_attrs.mq_maxmsg != m_attr_cache.mq_maxmsg) {
                    throw make_syserr(std::errc::invalid_argument,
                                      "mq_maxmsg mismatch - existing message queue created with a different mq_maxmsg");
                }

                if (new_attrs.mq_msgsize != m_attr_cache.mq_msgsize) {
                    throw make_syserr(std::errc::invalid_argument,
                                      "mq_msgsize mismatch - existing message queue created with different mq_msgsize");
                }
            }
            else
            {
                /* open the queue with default attrs. This will also inherit attrs if the
                 * queue was previously opened */
                m_mq_descriptor = ::mq_open(m_name.c_str(), m_flags, perms, nullptr);

                if (m_mq_descriptor == -1)
                    throw make_syserr(errno, "ctor::mq_open");

                /* Note that this saves the attrs to m_attr_cache */
                if (::mq_getattr(m_mq_descriptor, &m_attr_cache) == -1)
                    throw make_syserr(errno, "ctor::mq_getattr");
            }

            if (m_mq_descriptor == -1)
            {
                /* an error case might exist when we're creating a queue but
                 * want to know if one exists first.
                 */
                if (errno == EEXIST &&
                    ((m_flags & MQ_ERROR_ON_EXIST) == MQ_ERROR_ON_EXIST))
                {
                    throw make_syserr(std::errc::file_exists,
                                      "message_queue already exists with this name on the system.");
                }

                /* other real errors */
                throw make_syserr(errno, "ctor::mq_open");
            }

            /* I wish there was more fined-grained control for flushing..
             * Currently, if flushable(), flush happens both in ctor and dtor.
             */
            if (flushable())
                flush();
        }

        message_queue::~message_queue()
        {
#ifndef NDEBUG
            auto lm = get_message_count();
#endif
            if (flushable())
                flush();

            if (::mq_close(m_mq_descriptor) == -1) {
#if defined(MQ_LOG_STDERR)
                std::fprintf(stderr, "Failed to close message queue descriptor, this may result in undefined behaviour\n");
#endif
                return;
            }

            if (m_flags & MQ_UNLINK_ON_CLOSE)
            {
#ifndef NDEBUG
                if (lm && ((m_flags & MQ_FLUSH) == 0))
                    std::fprintf(stderr, "Unlinking mq with unread messages: %ld\n", lm);
#endif
                if (::mq_unlink(m_name.c_str()) == -1) {
#if defined(MQ_LOG_STDERR)
                    std::fprintf(stderr, "Failed to unlink message queue, this may result in undefined behaviour\n");
#endif
                    return;
                }
            }
        }

        std::error_code message_queue::send(const buffer_type_ptr message, std::size_t len,
                                            unsigned int prio)
        {
            auto ret = ::mq_send(m_mq_descriptor, message, len, prio);

            return ret == -1 ? make_error(errno) : std::error_code{};
        }

        /**
         * @brief Receives a message from the message queue. See recv_buffer_size().
         * @details Note that the man page for mq_recv() states that the buffer must
         *          be greater than or equal to the MQ's mq_msgsize attribute.
         * @param dest Destination buffer of at least recv_buffer_size() size
         * @param found_prio The priority of the recv'd message or nullptr
         * @return Pair of possible error code and size received.
         */
        std::pair<std::error_code, int> message_queue::recv(buffer_type_ptr dest,
                                                            unsigned int * found_prio)
        {
            auto ret = ::mq_receive(m_mq_descriptor, dest, m_attr_cache.mq_msgsize,
                                    found_prio);

            return ret == -1 ?
                std::make_pair(make_error(errno), ret) :
                std::make_pair(std::error_code{}, ret);
        }

        /**
         * @return Current message count of the message queue
         */
        long message_queue::get_message_count()
        {
            mq_attr props = {0};

            if (::mq_getattr(m_mq_descriptor, &props) == -1)
                throw make_syserr(errno, "get_message_count()::mq_getattr");

            return props.mq_curmsgs;
        }

    } /* end namespace v1 */

} /* end namespace ipc */

