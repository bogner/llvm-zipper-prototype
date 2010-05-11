//===----------------------------------------------------------------------===//
//
// ��������������������The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// <system_error>

// enum errc {...}

#include <system_error>

int main()
{
    static_assert(std::errc::address_family_not_supported == EAFNOSUPPORT, "");
    static_assert(std::errc::address_in_use == EADDRINUSE, "");
    static_assert(std::errc::address_not_available == EADDRNOTAVAIL, "");
    static_assert(std::errc::already_connected == EISCONN, "");
    static_assert(std::errc::argument_list_too_long == E2BIG, "");
    static_assert(std::errc::argument_out_of_domain == EDOM, "");
    static_assert(std::errc::bad_address == EFAULT, "");
    static_assert(std::errc::bad_file_descriptor == EBADF, "");
    static_assert(std::errc::bad_message == EBADMSG, "");
    static_assert(std::errc::broken_pipe == EPIPE, "");
    static_assert(std::errc::connection_aborted == ECONNABORTED, "");
    static_assert(std::errc::connection_already_in_progress == EALREADY, "");
    static_assert(std::errc::connection_refused == ECONNREFUSED, "");
    static_assert(std::errc::connection_reset == ECONNRESET, "");
    static_assert(std::errc::cross_device_link == EXDEV, "");
    static_assert(std::errc::destination_address_required == EDESTADDRREQ, "");
    static_assert(std::errc::device_or_resource_busy == EBUSY, "");
    static_assert(std::errc::directory_not_empty == ENOTEMPTY, "");
    static_assert(std::errc::executable_format_error == ENOEXEC, "");
    static_assert(std::errc::file_exists == EEXIST, "");
    static_assert(std::errc::file_too_large == EFBIG, "");
    static_assert(std::errc::filename_too_long == ENAMETOOLONG, "");
    static_assert(std::errc::function_not_supported == ENOSYS, "");
    static_assert(std::errc::host_unreachable == EHOSTUNREACH, "");
    static_assert(std::errc::identifier_removed == EIDRM, "");
    static_assert(std::errc::illegal_byte_sequence == EILSEQ, "");
    static_assert(std::errc::inappropriate_io_control_operation == ENOTTY, "");
    static_assert(std::errc::interrupted == EINTR, "");
    static_assert(std::errc::invalid_argument == EINVAL, "");
    static_assert(std::errc::invalid_seek == ESPIPE, "");
    static_assert(std::errc::io_error == EIO, "");
    static_assert(std::errc::is_a_directory == EISDIR, "");
    static_assert(std::errc::message_size == EMSGSIZE, "");
    static_assert(std::errc::network_down == ENETDOWN, "");
    static_assert(std::errc::network_reset == ENETRESET, "");
    static_assert(std::errc::network_unreachable == ENETUNREACH, "");
    static_assert(std::errc::no_buffer_space == ENOBUFS, "");
    static_assert(std::errc::no_child_process == ECHILD, "");
    static_assert(std::errc::no_link == ENOLINK, "");
    static_assert(std::errc::no_lock_available == ENOLCK, "");
    static_assert(std::errc::no_message_available == ENODATA, "");
    static_assert(std::errc::no_message == ENOMSG, "");
    static_assert(std::errc::no_protocol_option == ENOPROTOOPT, "");
    static_assert(std::errc::no_space_on_device == ENOSPC, "");
    static_assert(std::errc::no_stream_resources == ENOSR, "");
    static_assert(std::errc::no_such_device_or_address == ENXIO, "");
    static_assert(std::errc::no_such_device == ENODEV, "");
    static_assert(std::errc::no_such_file_or_directory == ENOENT, "");
    static_assert(std::errc::no_such_process == ESRCH, "");
    static_assert(std::errc::not_a_directory == ENOTDIR, "");
    static_assert(std::errc::not_a_socket == ENOTSOCK, "");
    static_assert(std::errc::not_a_stream == ENOSTR, "");
    static_assert(std::errc::not_connected == ENOTCONN, "");
    static_assert(std::errc::not_enough_memory == ENOMEM, "");
    static_assert(std::errc::not_supported == ENOTSUP, "");
    static_assert(std::errc::operation_canceled == ECANCELED, "");
    static_assert(std::errc::operation_in_progress == EINPROGRESS, "");
    static_assert(std::errc::operation_not_permitted == EPERM, "");
    static_assert(std::errc::operation_not_supported == EOPNOTSUPP, "");
    static_assert(std::errc::operation_would_block == EWOULDBLOCK, "");
    static_assert(std::errc::owner_dead == EOWNERDEAD, "");
    static_assert(std::errc::permission_denied == EACCES, "");
    static_assert(std::errc::protocol_error == EPROTO, "");
    static_assert(std::errc::protocol_not_supported == EPROTONOSUPPORT, "");
    static_assert(std::errc::read_only_file_system == EROFS, "");
    static_assert(std::errc::resource_deadlock_would_occur == EDEADLK, "");
    static_assert(std::errc::resource_unavailable_try_again == EAGAIN, "");
    static_assert(std::errc::result_out_of_range == ERANGE, "");
    static_assert(std::errc::state_not_recoverable == ENOTRECOVERABLE, "");
    static_assert(std::errc::stream_timeout == ETIME, "");
    static_assert(std::errc::text_file_busy == ETXTBSY, "");
    static_assert(std::errc::timed_out == ETIMEDOUT, "");
    static_assert(std::errc::too_many_files_open_in_system == ENFILE, "");
    static_assert(std::errc::too_many_files_open == EMFILE, "");
    static_assert(std::errc::too_many_links == EMLINK, "");
    static_assert(std::errc::too_many_symbolic_link_levels == ELOOP, "");
    static_assert(std::errc::value_too_large == EOVERFLOW, "");
    static_assert(std::errc::wrong_protocol_type == EPROTOTYPE, "");
}
