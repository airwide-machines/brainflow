#include <chrono>
#include <stdint.h>
#include <string.h>

#include "custom_cast.h"
#include "novaxr.h"
#include "openbci_helpers.h"
#include "timestamp.h"

#ifndef _WIN32
#include <errno.h>
#endif

constexpr int NovaXR::transaction_size;
constexpr int NovaXR::num_packages;
constexpr int NovaXR::package_size;
constexpr int NovaXR::num_channels;

NovaXR::NovaXR (struct BrainFlowInputParams params) : Board ((int)NOVAXR_BOARD, params)
{
    this->socket = NULL;
    this->is_streaming = false;
    this->keep_alive = false;
    this->initialized = false;
    this->start_time = 0.0;
    this->state = SYNC_TIMEOUT_ERROR;
}

NovaXR::~NovaXR ()
{
    skip_logs = true;
    release_session ();
}

int NovaXR::prepare_session ()
{
    if (initialized)
    {
        safe_logger (spdlog::level::info, "Session is already prepared");
        return STATUS_OK;
    }
    if (params.ip_address.empty ())
    {
        safe_logger (spdlog::level::info, "use default IP address 192.168.4.1");
        params.ip_address = "192.168.4.1";
    }
    if (params.ip_protocol == (int)IpProtocolType::TCP)
    {
        safe_logger (spdlog::level::err, "ip protocol is UDP for novaxr");
        return INVALID_ARGUMENTS_ERROR;
    }
    socket = new SocketClient (params.ip_address.c_str (), 2390, (int)SocketType::UDP);
    int res = socket->connect (NovaXR::transaction_size);
    if (res != (int)SocketReturnCodes::STATUS_OK)
    {
        safe_logger (spdlog::level::err, "failed to init socket: {}", res);
        delete socket;
        socket = NULL;
        return GENERAL_ERROR;
    }
    // force default settings for device
    res = config_board ("d");
    if (res != STATUS_OK)
    {
        safe_logger (spdlog::level::err, "failed to apply default settings");
        delete socket;
        socket = NULL;
        return BOARD_WRITE_ERROR;
    }
    initialized = true;
    return STATUS_OK;
}

int NovaXR::config_board (char *config)
{
    safe_logger (spdlog::level::debug, "Trying to config NovaXR with {}", config);
    int res = validate_config (config);
    if (res != STATUS_OK)
    {
        return res;
    }
    int len = strlen (config);
    res = socket->send (config, len);
    if (len != res)
    {
        if (res == -1)
        {
#ifdef _WIN32
            safe_logger (spdlog::level::err, "WSAGetLastError is {}", WSAGetLastError ());
#else
            safe_logger (spdlog::level::err, "errno {} message {}", errno, strerror (errno));
#endif
        }
        safe_logger (spdlog::level::err, "Failed to config a board");
        return BOARD_WRITE_ERROR;
    }
    return STATUS_OK;
}

int NovaXR::start_stream (int buffer_size, char *streamer_params)
{
    if (is_streaming)
    {
        safe_logger (spdlog::level::err, "Streaming thread already running");
        return STREAM_ALREADY_RUN_ERROR;
    }
    if (buffer_size <= 0 || buffer_size > MAX_CAPTURE_SAMPLES)
    {
        safe_logger (spdlog::level::err, "invalid array size");
        return INVALID_BUFFER_SIZE_ERROR;
    }

    if (db)
    {
        delete db;
        db = NULL;
    }
    if (streamer)
    {
        delete streamer;
        streamer = NULL;
    }

    int res = prepare_streamer (streamer_params);
    if (res != STATUS_OK)
    {
        return res;
    }
    db = new DataBuffer (NovaXR::num_channels, buffer_size);
    if (!db->is_ready ())
    {
        safe_logger (spdlog::level::err, "unable to prepare buffer");
        delete db;
        db = NULL;
        return INVALID_BUFFER_SIZE_ERROR;
    }

    // start streaming
    res = socket->send ("b", 1);
    if (res != 1)
    {
        if (res == -1)
        {
#ifdef _WIN32
            safe_logger (spdlog::level::err, "WSAGetLastError is {}", WSAGetLastError ());
#else
            safe_logger (spdlog::level::err, "errno {} message {}", errno, strerror (errno));
#endif
        }
        safe_logger (spdlog::level::err, "Failed to send a command to board");
        return BOARD_WRITE_ERROR;
    }
    start_time = get_timestamp ();

    keep_alive = true;
    streaming_thread = std::thread ([this] { this->read_thread (); });
    // wait for data to ensure that everything is okay
    std::unique_lock<std::mutex> lk (this->m);
    auto sec = std::chrono::seconds (1);
    if (cv.wait_for (lk, 5 * sec, [this] { return this->state != SYNC_TIMEOUT_ERROR; }))
    {
        this->is_streaming = true;
        return this->state;
    }
    else
    {
        Board::board_logger->error ("no data received in 5sec, stopping thread");
        this->is_streaming = true;
        this->stop_stream ();
        // more likely error occured due to wrong ip address, return UNABLE_TO_OPEN_PORT instead
        // SYNC_TIMEOUT_ERROR
        return UNABLE_TO_OPEN_PORT_ERROR;
    }
}

int NovaXR::stop_stream ()
{
    if (is_streaming)
    {
        keep_alive = false;
        is_streaming = false;
        streaming_thread.join ();
        if (streamer)
        {
            delete streamer;
            streamer = NULL;
        }
        this->state = SYNC_TIMEOUT_ERROR;
        int res = socket->send ("s", 1);
        if (res != 1)
        {
            if (res == -1)
            {
#ifdef _WIN32
                safe_logger (spdlog::level::err, "WSAGetLastError is {}", WSAGetLastError ());
#else
                safe_logger (spdlog::level::err, "errno {} message {}", errno, strerror (errno));
#endif
            }
            safe_logger (spdlog::level::err, "Failed to send a command to board");
            return BOARD_WRITE_ERROR;
        }
        return STATUS_OK;
    }
    else
    {
        return STREAM_THREAD_IS_NOT_RUNNING;
    }
}

int NovaXR::release_session ()
{
    if (initialized)
    {
        if (is_streaming)
        {
            stop_stream ();
        }
        initialized = false;
        if (socket)
        {
            socket->close ();
            delete socket;
            socket = NULL;
        }
    }
    return STATUS_OK;
}

void NovaXR::read_thread ()
{
    // eeg and emg channels were switch this format needs to be updated
    /* ------ NovaXR packet format --------
     * Packet Byte [0]:     Packet Number
     * Packet Byte [1:2]:   PPG
     * Packet Byte [3:4]:   EDA
     * Packet Byte [5:7]:   EEG_FC 0
     * Packet Byte [8:10]:  EEG_FC 1
     * Packet Byte [11:13]: EEG_OL 0
     * Packet Byte [14:16]: EEG_OL 1
     * Packet Byte [17:19]: EEG_OL 2
     * Packet Byte [20:22]: EEG_OL 3
     * Packet Byte [23:25]: EEG_OL 4
     * Packet Byte [26:28]: EEG_OL 5
     * Packet Byte [29:31]: EEG_OL 6
     * Packet Byte [32:34]: EEG_OL 7
     * Packet Byte [35:37]: EOG 0
     * Packet Byte [38:40]: EOG 1
     * Packet Byte [41:43]: EMG 0
     * Packet Byte [44:46]: EMG 1
     * Packet Byte [47:49]: EMG 2
     * Packet Byte [50:52]: EMG 3
     * Packet Byte [53]:    Battery Level range: 0-100
     * Packet Byte [54:55]: Skin Temperature
     * Packet Byte [56]:    Firmware Error Code
     * Packet Byte [64:71]: Timestamp
     * ----- Total 72 Bytes ------------
     */


    int res;
    unsigned char b[NovaXR::transaction_size];
    long counter = 0;
    double recv_avg_time = 0;
    double process_avg_time = 0;
    while (keep_alive)
    {
        auto recv_start_time = std::chrono::high_resolution_clock::now ();
        res = socket->recv (b, NovaXR::transaction_size);
        auto recv_stop_time = std::chrono::high_resolution_clock::now ();
        auto recv_duration =
            std::chrono::duration_cast<std::chrono::microseconds> (recv_stop_time - recv_start_time)
                .count ();

        if (res == -1)
        {
#ifdef _WIN32
            safe_logger (spdlog::level::err, "WSAGetLastError is {}", WSAGetLastError ());
#else
            safe_logger (spdlog::level::err, "errno {} message {}", errno, strerror (errno));
#endif
        }
        if (res != NovaXR::transaction_size)
        {
            safe_logger (spdlog::level::trace, "unable to read {} bytes, read {}",
                NovaXR::transaction_size, res);
            continue;
        }
        else
        {
            socket->send ("a", sizeof (char)); // send ack that data received
            // inform main thread that everything is ok and first package was received
            if (this->state != STATUS_OK)
            {
                safe_logger (spdlog::level::info,
                    "received first package with {} bytes streaming is started", res);
                {
                    std::lock_guard<std::mutex> lk (this->m);
                    this->state = STATUS_OK;
                }
                this->cv.notify_one ();
                safe_logger (spdlog::level::debug, "start streaming");
            }
        }

        auto processing_start_time = std::chrono::high_resolution_clock::now ();
        for (int cur_package = 0; cur_package < NovaXR::num_packages; cur_package++)
        {
            double package[NovaXR::num_channels] = {0.};
            int offset = cur_package * NovaXR::package_size;
            // package num
            package[0] = (double)b[0 + offset];
            // eeg and emg
            for (int i = 4, tmp_counter = 0; i < 20; i++, tmp_counter++)
            {
                // put them directly after package num in brainflow
                if (tmp_counter < 8)
                    package[i - 3] = eeg_scale_main_board *
                        (double)cast_24bit_to_int32 (b + offset + 5 + 3 * (i - 4));
                else if (tmp_counter < 14)
                    package[i - 3] =
                        emg_scale * (double)cast_24bit_to_int32 (b + offset + 5 + 3 * (i - 4));
                else
                    package[i - 3] = eeg_scale_sister_board *
                        (double)cast_24bit_to_int32 (b + offset + 5 + 3 * (i - 4));
            }
            int16_t temperature;
            int16_t ppg;
            int16_t eda;
            memcpy (&temperature, b + 54 + offset, 2);
            memcpy (&ppg, b + 1 + offset, 2);
            memcpy (&eda, b + 3 + offset, 2);
            package[17] = ppg;                    // ppg
            package[18] = eda;                    // eda
            package[19] = temperature / 100.0;    // temperature
            package[20] = (double)b[53 + offset]; // battery level

            double timestamp_device;
            memcpy (&timestamp_device, b + 64 + offset, 8);
            timestamp_device /= 1e6; // convert usec to sec
            double timestamp = timestamp_device + start_time;
            streamer->stream_data (package, NovaXR::num_channels, timestamp);
            db->add_data (timestamp, package);
        }
        auto processing_stop_time = std::chrono::high_resolution_clock::now ();
        auto processing_duration = std::chrono::duration_cast<std::chrono::microseconds> (
            processing_stop_time - processing_start_time)
                                       .count ();
        recv_avg_time += recv_duration / 1000.0;
        process_avg_time += processing_duration / 1000.0;
        counter++;
    }
    recv_avg_time /= counter;
    process_avg_time /= counter;
    safe_logger (spdlog::level::trace, "recv avg time in ms {}", recv_avg_time);
    safe_logger (spdlog::level::trace, "process avg time in ms {}", process_avg_time);
}
