#pragma once

#include <memory>
#include <functional>
#include <type_traits>

#include <boost/array.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/noncopyable.hpp>
#include <boost/date_time/posix_time/posix_time_duration.hpp>

#include "protocol/handler_factory.h"
#include "utils/dstring.h"
#include "utils/reusable_buffer.h"
#include "utils/log_wrapper.h"

namespace server
{

using interval = boost::posix_time::time_duration;
using berror_code = boost::system::error_code;
using tcp_socket = boost::asio::ip::tcp::socket;
using ssl_socket = boost::asio::ssl::stream<tcp_socket>;

class connector_interface
{
public:
	connector_interface() = default;
	connector_interface(const connector_interface&) = delete;
	connector_interface(connector_interface&&) = default;
	virtual ~connector_interface() = default;
	virtual void do_read() = 0;
	virtual void do_write() = 0;
	virtual boost::asio::ip::address origin() const = 0;
	virtual bool is_ssl() const noexcept = 0;
	reusable_buffer<MAXINBYTESPERLOOP> _rb;

protected:
	dstring _out;
};

// handler_interface will become a template!?
template <typename socket_type>
class connector: public connector_interface,
	public std::enable_shared_from_this< connector<socket_type> >
{
	std::shared_ptr<socket_type> _socket;
	handler_interface * _handler{nullptr};

	interval _handshake_ttl;
	interval _ttl;

	boost::asio::deadline_timer _timer;

	bool _writing {false};
	bool _stopped {false};

	void cancel_deadline() noexcept
	{
		LOGTRACE(this, " deadline canceled");
		berror_code err;
		_timer.cancel(err);
		if(err)
			LOGERROR(this," ", err.message());
	}

	void schedule_deadline( const interval &msec )
	{
		auto self = this->shared_from_this();
		_timer.expires_from_now(msec);
		_timer.async_wait([this,self](const berror_code&ec)
		{
			if(!ec)
			{
				LOGTRACE(this," deadline has expired");
				self->stop();
			}
			else if(ec != boost::system::errc::operation_canceled)
			{
				LOGERROR(this," error on deadline:", ec.message());
			}
		});
	}

public:
	/// Construct a connection with the given io_service.
	explicit connector(const interval& hto, const interval &ttl, std::shared_ptr<socket_type> socket) noexcept
		: _socket(std::move(socket))
		, _handshake_ttl(hto)
		, _ttl(ttl)
		, _timer(_socket->get_io_service())
	{
		LOGTRACE(this," constructor");
	}

	bool is_ssl() const noexcept override { return  std::is_same<socket_type, ssl_socket>::value; }

	~connector() noexcept
	{
		LOGTRACE(this," destructor start");
		if(_handler)
		{
			// handler_interface's derived classes will get an on_connector_nulled() event
			_handler->connector(nullptr);
			_handler = nullptr;
		}
		stop();
		LOGTRACE(this," destructor end");
	}

	boost::asio::ip::address origin() const override
	{
		boost::asio::ip::tcp::endpoint ep = _socket->lowest_layer().remote_endpoint();
		return ep.address();
	}

	//TODO: DRM-200:this method is required by ng_h2 apis, remove it once they'll be gone
	socket_type& socket() noexcept { return *_socket; }
	void handshake_countdown(){schedule_deadline(_handshake_ttl);}

	void renew_ttl() { schedule_deadline(_ttl); }

	void handler( handler_interface* h )
	{
		_handler = h;
		_handler->connector( this );
	}

	void start( bool tcp_no_delay = false )
	{
		assert( _handler );
		_socket->lowest_layer().set_option( boost::asio::ip::tcp::no_delay(tcp_no_delay) );
		if (_handler->start())
			do_read();
	}

	template<typename T = socket_type,typename std::enable_if<std::is_same<T, ssl_socket>::value, int>::type = 0>
	void stop() noexcept
	{
		if(!_stopped)
		{
			LOGTRACE(this," stopping");
			berror_code ec;
			_stopped = true;
			_timer.cancel(ec);
			_socket->lowest_layer().cancel(ec);
			_socket->shutdown(ec); // Shutdown - does it cause a TCP RESET?
		}
	}

	template<typename T = socket_type,typename std::enable_if<std::is_same<T, tcp_socket>::value, int>::type = 0>
	void stop() noexcept
	{
		if(!_stopped)
		{
			LOGTRACE(this," stopping");
			berror_code ec;
			_stopped = true;
			_timer.cancel(ec);
			_socket->lowest_layer().cancel(ec);
			_socket->shutdown(boost::asio::socket_base::shutdown_both, ec);
		}
	}

	void do_read() override
	{
		if(_stopped)
			return;

		renew_ttl();
		LOGTRACE(this," triggered a read");

		auto self = this->shared_from_this();
		auto buf = _rb.reserve();
		_socket->async_read_some( boost::asio::mutable_buffers_1(buf),
			[this,self](const berror_code& ec, size_t bytes_transferred)
			{
				cancel_deadline();
				if(!ec)
				{
					LOGDEBUG(this," received:",bytes_transferred," Bytes");
					assert(bytes_transferred);

					auto tmp = _rb.produce(bytes_transferred);
					if( _handler->on_read(tmp, bytes_transferred) )
					{
						LOGDEBUG(this," read succeded");
						do_read();
					}
					else
					{
						LOGDEBUG(this," error on_read - read failed");
						stop();
					}
				}
				else if(ec != boost::system::errc::operation_canceled)
				{
					LOGERROR(this," error during read: ", ec.message());
					stop();
				}
				else
				{
					LOGTRACE(this," read canceled");
				}
			});
	}

	void do_write() override
	{
		if (_writing || _stopped)
			return;

		_out = dstring{};
		if ( !_handler->on_write(_out) )
		{
			LOGERROR(this," error on_write - write failed");
			return stop();
		}

		if( !_out && _handler->should_stop() )
		{
			LOGDEBUG(this," nothing left to write, stopping");
			stop();
			return;
		}

		renew_ttl();

		if( !_out )
			return;

		LOGTRACE(this," triggered a write of ", _out.size(), " bytes");
		_writing = true;

		auto self = this->shared_from_this(); //Let the connector live inside the callback
		boost::asio::async_write(*_socket, boost::asio::buffer(_out.cdata(), _out.size()),
			[this, self](const berror_code& ec, size_t s)
			{
				cancel_deadline();
				_writing = false;
				if(!ec)
				{
					LOGDEBUG(this," correctly wrote ", s," bytes");
					do_write();
				}
				else if(ec != boost::system::errc::operation_canceled)
				{
					LOGERROR(this," error during write: ", ec.message());
					stop();
				}
				else
				{
					LOGTRACE(this," write canceled");
				}
			}
		);
	}
};

}
