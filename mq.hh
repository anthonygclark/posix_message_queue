#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

extern "C"
{
#include <mqueue.h>
#include <sys/types.h>
#include <fcntl.h>
#include <time.h>
}

namespace ipc
{
    inline namespace v1
    {
        /**
         * @brief Helper for throwing system_error...
         */
        inline std::system_error make_syserr(decltype(errno) e, const char * msg="")
        {
            return std::system_error(std::error_code(e, std::system_category()), msg);
        }

        /**
         * @brief Helper for throwing system_error...
         */
        inline std::system_error make_syserr(std::errc e, const char * msg="")
        {
            return std::system_error(static_cast<int>(e), std::system_category(), msg);
        }

        /**
         * @brief Exists since std::make_error_code doesnt take errno, it takes std::errc...
         * @param errno errno
         * @return associated error_code
         */
        inline std::error_code make_error(decltype(errno) e)
        {
            return std::error_code{errno, std::generic_category()};
        }

        namespace detail
        {
            extern unsigned long mq_proc_sys_fs_mqueue_msg_default;
            extern unsigned long mq_proc_sys_fs_mqueue_msg_max;
            extern unsigned long mq_proc_sys_fs_mqueue_msgsize_default;
            extern unsigned long mq_proc_sys_fs_mqueue_msgsize_max;
            extern unsigned long mq_proc_sys_fs_mqueue_queues_max;
            extern unsigned long mq_max_priority;
            extern unsigned long mq_min_priority;

            /**
             * @brief Reads an integral from a procfs file
             * @todo More error checking
             * @tparam T Return integral type
             * @param path The file path
             * @return File contents
             */
            template<typename T>
                T read_procfs_file(const char * path)
                {
                    assert(path);

                    static_assert(std::is_integral<T>::value,
                                  "T must be integral since procfs files contain only integrals (for this use)");

                    T ret;

                    auto f = std::fopen(path, "rb");

                    if (!f) {
                        throw make_syserr(std::errc::bad_address, "Failed to open /proc file");
                    }

                    if (std::fscanf(f, "%lu", &ret) != 1)
                    {
                        std::fclose(f);
                        throw make_syserr(std::errc::invalid_argument, "Failed to read value from file");
                    }

                    std::fclose(f);

                    return ret;
                }

        } /* end namespace detail */

        /**
         * @brief Wrapper around posix message queue (mq_overview(7))
         */
        class message_queue final
        {
        public:
            /**< Type used to represent a buffer that can be used as I/O with the message queue. */
            using buffer_type = char;
            /**< Convenience */
            using buffer_type_ptr = std::add_pointer<buffer_type>::type;

            /* Default user owner only can use the queue */
            constexpr static const mode_t default_permissions = 0700;

            /**
             * \enum flags
             * Note that all RW flags have O_CREAT. This is to uncouple the idea
             * of parent/child (creator/slave, master/slave) relationships.
             * The creator is the one who opens first.
             */
            enum flags : unsigned int
            {
                MQ_READ            = O_CREAT | O_CLOEXEC | O_RDONLY,
                MQ_RW              = O_CREAT | O_CLOEXEC | O_RDWR,
                MQ_WRITE           = O_CREAT | O_CLOEXEC | O_WRONLY,
                /**< Mainly used single-mode where a single message queue is used for both send and recv */
                MQ_ERROR_ON_EXIST  = O_EXCL,
                MQ_NONBLOCK        = O_NONBLOCK,
                /**< Removes file handle when message_queue is closed */
                MQ_UNLINK_ON_CLOSE = 0x0800000,
                /**< If a message_queue is opened and contains a read flag, this
                 * will flush out all unread messages at construction and destruction time */
                MQ_FLUSH           = 0x1000000,
            };

        private:
            /**< Currently used flags. Since the actual message_queue
             * api uses unsigned int, we just store it to operate on it
             * via bitwise operations. */
            std::underlying_type<flags>::type m_flags;
            /**< Filename/handle */
            std::string m_name;
            /**< queue descriptor */
            mqd_t m_mq_descriptor;

            /**< Copy of the attributes the MQ was opened with.
             * Note, you should not use curmsgs from here as
             * the value is typically stale.
             */
            mq_attr m_attr_cache;

            /**
             * @brief MQ flush implementation. Calls recv and throws away all
             *          current messages. Note, uses new[recv_buffer_size()]
             * @todo avoid allocation.
             */
            void flush();

        public:
            /**
             * @brief Simple way to do some checking that mq_open does on the queue name.
             * @param name The name of the queue
             * @return T if valid, F other wise
             */
            static bool name_is_valid(std::string const & name);

            /**
             * @return If the MQ should be able to flush
             */
            bool flushable() const
            {
                return ((m_flags & MQ_FLUSH) == MQ_FLUSH)
                    && ((m_flags & MQ_READ) == MQ_READ);
            }

            /**
             * @brief Constructor
             * @details Opens the message queue
             * @param name The name of the message queue
             * @param flags MQ flags - controls operation capabilities
             * @param perms Permissions for the created queue
             * @param max_messages Override for maximum messages. Default: sysconf
             * @param max_message_size Override for maximum message size. Default: sysconf
             */
            message_queue(std::string const & name, unsigned int flags,
                          mode_t perms = default_permissions,
                          long max_messages = -1, long max_message_size = -1);

            /**
             * @brief Destructor.
             * Flushes and/or unlinks the queue if the flags allow
             */
            ~message_queue();

            /**
             * @brief Sends a message to the message queue.
             * @param message The message to send
             * @param len Message length, must be <= the mq_msgsize of the queue.
             * @param prio Priority of the message. See constructor.
             * @return Possible error
             */
            std::error_code send(const buffer_type_ptr message, std::size_t len,
                                 unsigned int prio = detail::mq_min_priority);

            /**
             * @brief Sends a message to the message queue with a timeout
             * @details Take special care when MQ_NONBLOCK is set as this call will not block
             * @tparam Ts Types to forward to timeout
             * @param message The message to send
             * @param len Message length, must be <= the mq_msgsize of the queue.
             * @param timeout Timeout in duration, see other overload supporting time_point
             * @param prio Priority of the message. See constructor.
             * @return Possible error
             */
            template<typename... Ts>
                std::error_code send(const buffer_type_ptr message, std::size_t len,
                                     std::chrono::duration<Ts...> timeout,
                                     unsigned int prio = detail::mq_min_priority)
                {
                    ::timespec ts = {0};
                    auto const one_billion = 1000000000;

                    ts.tv_sec = std::time_t{std::chrono::duration_cast<std::chrono::seconds>(timeout).count()};
                    ts.tv_nsec = (std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count() % one_billion);

                    auto ret = ::mq_timedsend(m_mq_descriptor, message, len, prio, &ts);

                    return ret == -1 ? make_error(errno) : std::error_code{};
                }

            /**
             * @brief Sends a message to the message queue with a timeout
             * @details Take special care when MQ_NONBLOCK is set as this call will not block
             * @tparam Ts Types to forward to timeout
             * @param message The message to send
             * @param len Message length, must be <= the mq_msgsize of the queue.
             * @param timeout Timeout in time_point. Time since epoch is used.
             * @param prio Priority of the message. See constructor.
             * @return Possible error
             */
            template<typename... Ts>
                std::error_code send(const buffer_type_ptr message, std::size_t len,
                                     std::chrono::time_point<Ts...> timeout,
                                     unsigned int prio = detail::mq_min_priority)
                {
                    return send(message, len, timeout.time_since_epoch(), prio);
                }

            /**
             * @brief Receives a message from the message queue. See recv_buffer_size().
             * @details Note that the man page for mq_recv() states that the buffer must
             *          be greater than or equal to the MQ's mq_msgsize attribute.
             * @param dest Destination buffer of at least recv_buffer_size() size
             * @param found_prio The priority of the recv'd message or nullptr
             * @return Pair of possible error code and size received.
             */
            std::pair<std::error_code, int> recv(buffer_type_ptr dest,
                                                 unsigned int * found_prio = nullptr);

            /**
             * @brief Receives a message from the message queue. See recv_buffer_size().
             * @details Note that the man page for mq_recv() states that the buffer must
             *          be greater than or equal to the MQ's mq_msgsize attribute.
             * @param dest Destination buffer of at least recv_buffer_size() size
             * @param timeout Timeout in duration. See overload supporting time_point
             * @param found_prio The priority of the recv'd message or nullptr
             * @return Pair of possible error code and size received.
             */
            template<typename... Ts>
                std::pair<std::error_code, int> recv(buffer_type_ptr dest,
                                                     std::chrono::duration<Ts...> timeout,
                                                     unsigned int * found_prio = nullptr)
                {
                    ::timespec ts = {0};
                    auto const one_billion = 1000000000;

                    ts.tv_sec = std::time_t{std::chrono::duration_cast<std::chrono::seconds>(timeout).count()};
                    ts.tv_nsec = (std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count() % one_billion);

                    auto ret = ::mq_timedreceive(m_mq_descriptor, dest, m_attr_cache.mq_msgsize,
                                                 found_prio, &ts);

                    return ret == -1 ?
                        std::make_pair(make_error(errno), ret) :
                        std::make_pair(std::error_code{}, ret);

                }

            /**
             * @brief Receives a message from the message queue. See recv_buffer_size().
             * @details Note that the man page for mq_recv() states that the buffer must
             *          be greater than or equal to the MQ's mq_msgsize attribute.
             * @param dest Destination buffer of at least recv_buffer_size() size
             * @param timeout Timeout in time_point. Time since epoch is used.
             * @param found_prio The priority of the recv'd message or nullptr
             * @return Pair of possible error code and size received.
             */
            template<typename... Ts>
                std::pair<std::error_code, int> recv(buffer_type_ptr dest,
                                                     std::chrono::time_point<Ts...> timeout,
                                                     unsigned int * found_prio = nullptr)
                {
                    return recv(dest, timeout.time_since_epoch(), found_prio);
                }

            /**
             * @return Retreives the buffer size needed to receive a message from this message queue
             */
            long recv_buffer_size() const
            {
                return m_attr_cache.mq_msgsize;
            }

            /**
             * @return Current message count of the message queue
             */
            long get_message_count();
        };

    } /* end namespace v1 */

} /* end namespace ipc */
