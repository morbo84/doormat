#ifndef DOOR_MAT_CLIENT_WRAPPER_H
#define DOOR_MAT_CLIENT_WRAPPER_H

#include "../chain_of_responsibility/node_interface.h"
#include "../service_locator/service_locator.h"
#include "../board/abstract_destination_provider.h"
#include "../utils/log_wrapper.h"
#include "../http/http_codec.h"
#include "../errors/error_codes.h"
#include "../log/access_record.h"
#include "../utils/reusable_buffer.h"
#include "../network/communicator.h"

#include <chrono>
#include <boost/asio.hpp>

namespace nodes
{

class client_wrapper : public node_interface
{
	using node_interface::node_interface;

	std::chrono::time_point<std::chrono::high_resolution_clock> start;
public:
	client_wrapper(req_preamble_fn request_preamble,
		req_chunk_fn request_body,
		req_trailer_fn request_trailer,
		req_canceled_fn request_canceled,
		req_finished_fn request_finished,
		header_callback hcb,
		body_callback body,
		trailer_callback trailer,
		end_of_message_callback end_of_message,
		error_callback ecb,
		response_continue_callback rccb,
		logging::access_recorder *aclogger = nullptr);

	/** events from the chain to which the client wrapper responds. */
	void on_request_preamble(http::http_request&& message);
	void on_request_body(dstring&& chunk);
	void on_request_trailer(dstring&& k, dstring&& v);
	void on_request_canceled(const errors::error_code &err);
	void on_request_finished();


	/** procedure to be performed when the socket is acquired. */
	void on_connect(std::unique_ptr<boost::asio::ip::tcp::socket> socket);

	~client_wrapper();

private:

	class communicator_proxy
	{
		//todo: replace with optional as soon as possible.
		std::unique_ptr<network::communicator> _communicator{nullptr};
		dstring temporary_string;
	public:
		void enqueue_for_write(dstring d)
		{
			if(_communicator)
				_communicator->write(std::move(d));
			else
				temporary_string.append(d);
		}

		void set_communicator(network::communicator *comm)
		{
			assert(!_communicator);
			_communicator = std::unique_ptr<network::communicator>{comm};
			_communicator->start();
			if(temporary_string.size())
				_communicator->write( std::move(temporary_string) );

			//Not sure that's needed
			temporary_string = {};
		}

		void shutdown_communicator()
		{
			if(_communicator) _communicator->stop();
		}

	} write_proxy;

	/**
	 * @brief termination_handler checks whether a termination condition (an error or an EOM)
	 * has been triggered and handles the communication of the relative event back to the chain.
	 *
	 * As it can possibly call the on_error() or on_end_of_message() member functions,
	 * it may destroy the whole chain and the current instance of client_wrapper,
	 * so be carefull.
	 *
	 * @return true if at least a termination condition holds, false otherwise.
	 */
	void termination_handler();

	void stop();

	void connect();

	static routing::abstract_destination_provider::address get_custom_address(const http::http_request &req);

	bool stopping{false};
	bool finished_request{false};
	bool finished_response{false};
	bool managing_continue{false}; //current state; used to manage HTTP 1.1 / 100 status codes.
	bool canceled{false};

	errors::error_code errcode;

	routing::abstract_destination_provider::address addr;
	bool custom_addr{false};

	uint8_t connection_attempts{0};

	http::http_codec codec;
	//message received from the decoder
	http::http_response received_parsed_message;
	//used in order to manage connect.
	http::http_request local_request;

	uint8_t waiting_count{0};
};



} // namespace nodes


#endif //DOOR_MAT_CLIENT_WRAPPER_H
