#include <iostream>
#include <chrono>
#include <cstring>

#include "mq.hh"

using namespace ipc;

int main()
{
    std::cout << std::boolalpha << "'/anthony' should be true: "
        << message_queue::name_is_valid("/anthony\0") << std::endl;

    std::cout << std::boolalpha << "'/anthony/' should be false: "
        << message_queue::name_is_valid("/anthony/\0") << std::endl;

    std::cout << std::boolalpha << "'/' should be false: "
        << message_queue::name_is_valid("/\0") << std::endl;

    std::cout << std::boolalpha << "'' should be false: "
        << message_queue::name_is_valid("\0") << std::endl;

    /* Sender queue */
    message_queue sender {
        "/anthony",
            message_queue::flags::MQ_WRITE
                | message_queue::flags::MQ_UNLINK_ON_CLOSE
                , message_queue::default_permissions
                , 2 /* 2 Max messages */
                , 1024 /* 1024 max size */
    };

    /* Create buffer... */
    auto buf = new message_queue::buffer_type[sender.recv_buffer_size()];
    /* ... zero it */
    std::memset(buf, 0, sender.recv_buffer_size());

    /* Put some bits in buf */
    std::memcpy(buf, "test", 4);

    /* Send buf max times to test FLUSH */
    auto s = sender.send(buf, 4);
    s = sender.send(buf, 4);

    /* Try a timed send... let it fail since there
     * is already 2 messages in the queue. */
    s = sender.send(buf, 4,
                    std::chrono::system_clock::now() + std::chrono::seconds(1)
                   );

    if (s == std::errc::timed_out) {
        std::printf("Sender timed out like we expected!\n");
    }

    /* Receiver queue, inherits limits from sender */
    message_queue receiver {
        "/anthony",
            message_queue::flags::MQ_READ
                | message_queue::flags::MQ_FLUSH
    };

    /* Try to send another time to see if FLUSH worked... If it didn't,
     * this would block */
    s = sender.send(buf, 4);
    std::printf("Sender has %ld messages\n", sender.get_message_count());

    /* Receive the send 2 lines above */
    auto r = receiver.recv(buf);

    /* Use streams here to use operator<< for error_code.. */
    std::cout << "Sender   - Error: " << s << std::endl;
    std::cout << "Receiver - Error: " << r.first << ", Bytes: " << r.second << ", Msg: " << buf << std::endl;

    /* Try recv one more time to test timeout */
    r = receiver.recv(buf,
                      std::chrono::system_clock::now() + std::chrono::seconds(1)
                     );

    if (r.first == std::errc::timed_out) {
        std::printf("Reciever timed out like we expected!\n");
    }

    delete[] buf;

}
