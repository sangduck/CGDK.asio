﻿//*****************************************************************************
//*                                                                           *
//*                      Cho sanghyun's Game Classes II                       *
//*                                                                           *
//*                          asio.ex network classes                          *
//*                                                                           *
//*                                                                           *
//*                                                                           *
//*                                                                           *
//*  This Program is programmed by Cho SangHyun. sangduck@cgcii.co.kr         *
//*  Best for Game Developement and Optimized for Game Developement.          *
//*                                                                           *
//*                (c) 2019. Cho Sanghyun. All right reserved.                *
//*                          http://www.CGCII.co.kr                           *
//*                                                                           *
//*****************************************************************************
#pragma once

class CGDK::asio::Nsocket_tcp : virtual public Isocket_tcp, public messageable
{
public:
			Nsocket_tcp();
	virtual ~Nsocket_tcp() noexcept;

protected:
	virtual void on_request_connect() {}
	virtual void on_connect() {}
	virtual void on_fail_connect(boost::system::error_code /*_error_code*/) noexcept {}
	virtual void on_disconnect(boost::system::error_code /*_error_code*/) noexcept {}
	virtual int on_message(sMESSAGE& /*_msg*/) { return 0; }

	virtual void process_request_connect() override;
	virtual void process_fail_connect(boost::system::error_code _error_code) noexcept override;
	virtual void process_complete_connect() override;
	virtual void process_closesocket(boost::system::error_code _error_code) noexcept override;
	virtual bool process_send(SEND_NODE&& _send_node) override;
			bool process_send_sync(SEND_NODE&& _send_node);
			void process_receive_async();
	virtual size_t process_get_message_size(const CGDK::buffer_view& _buf) const { return _buf.front<uint32_t>(); }

public:
			shared_buffer m_received_msg;
			boost::asio::mutable_buffer m_asio_receiving_msg;
			std::shared_ptr<Isocket_tcp> m_hold_async;
			shared_buffer::object_ptr_t m_hold_receiving;
			size_t m_message_size_min{ MIN_MESSAGE_SIZE };
			size_t m_message_size_max{ MAX_MESSAGE_SIZE };
};
