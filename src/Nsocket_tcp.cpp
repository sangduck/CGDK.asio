﻿#include "cgdk/asio.h"


CGDK::asio::Nsocket_tcp::Nsocket_tcp()
{
}

CGDK::asio::Nsocket_tcp::~Nsocket_tcp() noexcept
{
	// 1) complete close socket
	this->process_closesocket(boost::asio::error::connection_aborted);
}

void CGDK::asio::Nsocket_tcp::process_request_connect()
{
	this->on_request_connect();
}

void CGDK::asio::Nsocket_tcp::process_fail_connect(boost::system::error_code _error_code) noexcept
{
	this->on_fail_connect(_error_code);
}

void CGDK::asio::Nsocket_tcp::process_complete_connect()
{
	// 1) set socket state ESOCKET_STATUE::ESTABLISHED
	{
		// - desiged state
		ESOCKET_STATUE socket_state_old = ESOCKET_STATUE::SYN;

		// - change state
		auto changed = this->m_socket_state.compare_exchange_weak(socket_state_old, ESOCKET_STATUE::ESTABLISHED);

		// check)
		assert(changed == true);

		// return) 
		if (changed == false)
			return;
	}

	// statistics)
	this->m_time_connect = std::chrono::system_clock::now();

	// 2) on connect
	try
	{
		// - call 'on_connect'
		this->on_connect();
	}
	catch (...)
	{
		// - abortive close
		this->process_close_native_handle();

		// reraise)
		throw;
	}

	// 3) prepare receive
	{
		// - reset buffer
		this->m_received_msg = alloc_shared_buffer(RECEIVE_BUFFER_SIZE);
		this->m_asio_receiving_msg = boost::asio::mutable_buffer{ this->m_received_msg.data(), RECEIVE_BUFFER_SIZE };

		try
		{
			// - read async
			this->process_receive_async();
		}
		catch (...)
		{
			// - close socket!
			this->process_closesocket(boost::asio::error::operation_aborted);

			// reraise)
			throw;
		}
	}
}

void CGDK::asio::Nsocket_tcp::process_closesocket(boost::system::error_code _error_code) noexcept
{
	// 1) abortive close
	this->process_close_native_handle();

	// 2) set socket state ESOCKET_STATUE::CLOSING
	{
		// - desiged state
		ESOCKET_STATUE socket_state_old = ESOCKET_STATUE::ESTABLISHED;

		// - change state
		auto changed = this->m_socket_state.compare_exchange_weak(socket_state_old, ESOCKET_STATUE::CLOSING);

		// return) 
		if (changed == false)
			return;
	}

	// statistics)
	++Nstatistics::statistics_connect_disconnect;
	--Nstatistics::statistics_connect_keep;

	// 3) set time
	this->m_time_disconnect = std::chrono::system_clock::now();

	// 4) call 'on disconnect'
	this->on_disconnect(_error_code);

	// 5) remove form acceptor
	this->process_connective_closesocket();

	// 6) free buffer
	this->m_received_msg.clear();

	// 7) set socket state ESOCKET_STATUE::NONE
	{
		// - ...
		auto old_socket_state = this->m_socket_state.exchange(ESOCKET_STATUE::NONE);

		// check)
		assert(old_socket_state == ESOCKET_STATUE::CLOSING);

		// check)
		if (old_socket_state != ESOCKET_STATUE::CLOSING)
			return;
	}
}

bool CGDK::asio::Nsocket_tcp::process_send(SEND_NODE&& _send_node)
{
	return this->process_send_sync(std::move(_send_node));
}

bool CGDK::asio::Nsocket_tcp::process_send_sync(SEND_NODE&& _send_node)
{
	// check) 
	if (this->m_socket_state < ESOCKET_STATUE::CLOSING)
		return false;

	// statistics)
	this->m_time_last_send = std::chrono::system_clock::now();

	// 1) buffer를 설정
	boost::asio::const_buffer buffer_transfer { _send_node.buf_send.data(), _send_node.buf_send.size()};

	// 2) 전송을 한다.
	while (buffer_transfer.size() != 0)
	{
		// declare)
		boost::system::error_code ec;

		// - send~
		auto size_transfered = this->m_socket.write_some(buffer_transfer, ec);

		// check) 
		if (ec)
			return false;

		// - proceed
		buffer_transfer += size_transfered;
	}

	// statistics)
	Nstatistics::statistics_send_messages += _send_node.message_count;
	Nstatistics::statistics_send_bytes += _send_node.buf_send.size();

	// return)
	return true;
}
void CGDK::asio::Nsocket_tcp::process_receive_async()
{
	// ----------------------------------------------------------------------------
	// 설명) 비동기 receive를 건다.
	// 
	// 전송 데이터를 받기 위해 receive 버퍼를 미러 걸어 놓는 작업과
	// 그 비동기 receive가 완료 되어 왔을 때 message를 처리하고 다시
	// receive를 거는 작업을 수행합니다.
	// 
	// ----------------------------------------------------------------------------
	// 1) hold self (비동기 receive 처리가 완료 될 때까지 객체의 소멸을 막기 위해 hold)
	if(!this->m_hold_async)
		this->m_hold_async = this->shared_from_this();

	// 2) hold receiving buffer(비동기 receive 처기가 완료 될 때까지 buffer가 할당해제 되지 않도록 하기 위해 hold)
	this->m_hold_receiving = this->m_received_msg.get_source();

	try
	{
		// 3) send async
		this->m_socket.async_read_some(this->m_asio_receiving_msg,
			[=, this](boost::system::error_code _error_code, std::size_t _length)
			{
				// - release receiving buffer
				this->m_hold_receiving.reset();

				// - disconnected?
				if (_error_code.failed())
				{
					// ----------------------------------------------------------------------------
					// 설명) error는 ...
					//  'boost::asio::error::eof' -> 정상접속 종료(graceful close)
					//  'boost::asio::error::connection_reset' -> 비정상 접속 종료(abortive close)
					// ----------------------------------------------------------------------------
					// - closesocket
					this->process_closesocket(_error_code);

					// - release
					this->m_hold_async.reset();

					// return) 
					return;
				}

				// declare)
				std::shared_ptr<Isocket_tcp> hold_async;

				// statistics)
				this->m_time_last_receives = std::chrono::system_clock::now();

				// - 임시로 복사
				auto temp_received = this->m_received_msg;

				// - 받은 만큼 길이를 더해준다.
				temp_received.add_size(_length);

				// declare)
				size_t count_messages = 0;

				// declare)
				sMESSAGE msg;

				try
				{
					// - get message size
					const auto min_message_size = this->m_message_size_min;
					const auto max_message_size = this->m_message_size_max;

					// loop) remained_size가 header_size보다 작으면 message 읽기 중단.
					while (temp_received.size() >= min_message_size)
					{
						// - get message size
						auto message_size = this->process_get_message_size(temp_received);

						// check) 
						if (message_size < min_message_size)
							throw std::length_error("message length is invalid");

						// check) 
						if (message_size > temp_received.size())
						{
							// check) 
							if (message_size > max_message_size)
								throw std::length_error("message length is invalid");

							// - release
							hold_async = std::move(this->m_hold_async);

							// break) 
							break;
						}

						// - make message ( 반드시 복사해야 한다. on_message 처리 중 값을 변화시킬 수 있으므로...)
						msg.buf_message = temp_received ^ message_size; // temp_received의 size만 message_size로 변경해서 msg.buf_message로 넣는다.

						// - on message
						this->process_message(msg);

						// statistics)
						++count_messages;

						// - 처리한 만큼 buffer를 줄인다.
						temp_received += offset(message_size);
					}

					// statistics) 
					Nstatistics::statistics_receive_bytes += _length;
					Nstatistics::statistics_receive_messages += count_messages;

					// - 
					this->m_received_msg = temp_received;
					this->m_asio_receiving_msg += _length;

					if (this->m_asio_receiving_msg.size() < MIN_MESSAGE_BUFFER_ROOM || this->m_asio_receiving_msg.size() < msg.buf_message.size())
					{
						// - 기본 메시지 buffer 크기 + 받아 놓은 데이터 크기
						auto size_new = RECEIVE_BUFFER_SIZE + temp_received.size();

						// - 만약 다음 메시지의 크기가 기본 메시지 buffer 크기보다 크면 메시지 크기 만큼을 더한다.
						if (msg.buf_message.size() > RECEIVE_BUFFER_SIZE)
							size_new += msg.buf_message.size();

						// - 새로운 buffer를 할 당받는다.
						auto buf_new = alloc_shared_buffer(size_new);

						// - 남아 있는 데이터가 있으면 새로운 buffer의 제일 앞에 복사한다.
						if (temp_received.size() != 0)
							memcpy(buf_new.data(), temp_received.data(), temp_received.size());

						// - 새로운 buffer를 설정한다.
						this->m_received_msg = buf_new ^ temp_received.size();
						this->m_asio_receiving_msg = boost::asio::mutable_buffer{ buf_new.data() + temp_received.size(), size_new - temp_received.size() };
					}

					// declare)
					bool is_closed = false;

					// - async receive
					{
						// - lock
						std::lock_guard cs(this->m_lock_socket);

						// - async receive
						if (this->m_socket.is_open() == true)
							this->process_receive_async();
						else
							is_closed = true;
					}

					if (is_closed)
					{
						// - close socket 
						this->process_closesocket(boost::asio::error::operation_aborted);

						// - release
						hold_async = std::move(this->m_hold_async);
					}
				}
				catch (...)
				{
					// - close socket 
					this->process_closesocket(boost::asio::error::operation_aborted);

					// - release
					hold_async = std::move(this->m_hold_async);
				}
			});
	}
	catch (...)
	{
		// - rollback
		this->m_hold_async.reset();

		// - rollback
		this->m_hold_receiving.reset();

		// reraise)
		throw;
	}
}
