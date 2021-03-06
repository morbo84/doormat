#include <cstring>

#include "session.h"
#include "stream.h"
#include "http2error.h"
#include "../utils/log_wrapper.h"
#include "http2alloc.h"

const std::size_t header_size_bytes = 8 * 1024;

namespace
{

// @todo Sooner or later it must be moved to configuration
static constexpr const std::int32_t max_concurrent_streams = 100;

}

namespace http2
{

void session::on_eom()
{
	LOGTRACE("on_eom does not look useful");
	if ( nghttp2_session_want_write( session_data.get() ) )
		do_write();
}

void session::on_error(const int& er)
{
	LOGERROR( "Session error: ", er );
	/// @todo goaway ?
	finished_stream();
}

session::session(): session_data( nullptr, [] ( nghttp2_session* s ) { if ( s ) nghttp2_session_del ( s ); } )
{
	LOGTRACE("Session: ", this );
	nghttp2_option_new( &options );
	nghttp2_option_set_peer_max_concurrent_streams( options, max_concurrent_streams );

	all = create_allocator();

	nghttp2_session_callbacks *callbacks;
	nghttp2_session_callbacks_new(&callbacks);
// 	nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
	nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks,
		on_frame_recv_callback);
	nghttp2_session_callbacks_set_on_stream_close_callback(
		callbacks, on_stream_close_callback);
	nghttp2_session_callbacks_set_on_header_callback(callbacks,
		on_header_callback);
	nghttp2_session_callbacks_set_on_begin_headers_callback(
		callbacks, on_begin_headers_callback);
	nghttp2_session_callbacks_set_on_data_chunk_recv_callback( callbacks,
		on_data_chunk_recv_callback );

	nghttp2_session_callbacks_set_on_frame_send_callback( callbacks, frame_send_callback );
	nghttp2_session_callbacks_set_on_frame_not_send_callback( callbacks, frame_not_send_callback );

	nghttp2_session* ngsession;
	nghttp2_session_server_new3( &ngsession, callbacks, this, options, &all );

	nghttp2_session_callbacks_del(callbacks);

	session_data.reset( ngsession );
}

bool session::start() noexcept
{
	LOGTRACE("start");
	if (connector() == nullptr)
		return false;
	send_connection_header();

	do_write();
	return true;
}

void session::send_connection_header()
{
	LOGTRACE("send_connection_header");
	constexpr const int ivlen = 1;
	nghttp2_settings_entry iv[ivlen] = { {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, max_concurrent_streams} };

	int r = nghttp2_submit_settings( session_data.get(), NGHTTP2_FLAG_NONE, iv, ivlen );

	// If this happens is a bug - die with fireworks
	if ( r ) THROW( errors::setting_connection_failure, r );

	do_write();
}

stream* session::create_stream ( std::int32_t id )
{
	stream* stream_data = static_cast<stream*>( all.malloc( sizeof(stream), all.mem_user_data ) );
	LOGTRACE(" Create stream: ", id, " address: ", stream_data );
	new ( stream_data ) stream{ this, [] ( stream* s, session* sess )
		{
			LOGTRACE("Stream destruction: ", s, " session: ", sess );
			s->~stream();
			sess->all.free( static_cast<void*>( s ), sess->all.mem_user_data  );
		} }; // The pointed object will commit suicide

	stream_data->id( id );
	int rv = nghttp2_session_set_stream_user_data( session_data.get(), id, stream_data );
	if ( rv != 0 ) LOGERROR ( "nghttp2_session_set_stream_user_data ",  nghttp2_strerror( rv ) );

	++stream_counter;
	LOGTRACE(" stream is ", stream_data, " stream counter ", stream_counter );
	return stream_data;
}

void session::go_away()
{
	std::int32_t last_id = nghttp2_session_get_last_proc_stream_id( session_data.get() );
	std::uint32_t error_code = NGHTTP2_INTERNAL_ERROR;
	int r = nghttp2_submit_goaway( session_data.get(), NGHTTP2_FLAG_NONE, last_id, error_code, nullptr, 0 );
	if ( r < 0 )
	{
		// No mem or invalid data - they look like a bug to me
//		LOGERROR( "nghttp2_submit_goaway ", nghttp2_strerror( r ) );
		THROW( errors::failing_at_failing, r );
	}

	gone = true;
}

int session::on_begin_headers_callback( nghttp2_session *session_, const nghttp2_frame *frame, void *user_data )
{
	LOGTRACE("on_begin_headers_callback - id: ", frame->hd.stream_id );
	session* s_this = static_cast<session*>( user_data );

	if ( frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST ) return 0;

	s_this->create_stream( frame->hd.stream_id );
	return 0;
}

int session::on_header_callback( nghttp2_session *session_,
	const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value,
	size_t valuelen, uint8_t flags, void *user_data )
{
	LOGTRACE("on_header_callback");
	session* s_this = static_cast<session*>( user_data );
	if ( s_this == nullptr )
	{
		LOGERROR( "s_this nullptr on_header_callback" );
		return 0;
	}

	static const char PATH[] = ":path";
	static const char METHOD[] = ":method";
	static const char AUTHORITY[] = ":authority";
	static const char SCHEME[] = ":scheme";
	switch (frame->hd.type)
	{
		case NGHTTP2_HEADERS:
			if (frame->headers.cat != NGHTTP2_HCAT_REQUEST) break;

			void* user_data = nghttp2_session_get_stream_user_data( session_, frame->hd.stream_id);
			stream* stream_data = static_cast<stream*>( user_data );
			if ( ! stream_data ) break;

			if ( namelen == sizeof(PATH) - 1 && memcmp(PATH, name, namelen) == 0 )
			{
				size_t j;
				for (j = 0; j < valuelen && value[j] != '?'; ++j); // Semicolon intended!
				dstring decoded_path(value, j);
				stream_data->path( decoded_path);

				size_t k = j;
				for (; j < valuelen && value[j] != '#'; ++j);
				dstring decoded_query(value + k, j - k);
				if ( ! decoded_query.empty() )
				{
					LOGTRACE("Session: ", s_this, " query: ", decoded_query );
					stream_data->query( decoded_query );
				}

				dstring decoded_fragment(value + k,valuelen - k);
				if ( ! decoded_fragment.empty() )
				{
					LOGTRACE("Session: ", s_this, " fragment: ", decoded_fragment );
					stream_data->fragment( decoded_fragment );
				}
			}
			else if ( namelen == sizeof(SCHEME) - 1 && memcmp(SCHEME, name, namelen ) == 0 )
			{
				dstring scheme { reinterpret_cast<const char*> ( value ), valuelen };
				stream_data->scheme( scheme );
			}
			else if ( namelen == sizeof(METHOD) - 1 && memcmp(METHOD, name, namelen) == 0 )
			{
				dstring method{reinterpret_cast<const char*> ( value ), valuelen };
				stream_data->method( method );
			}
			else if ( namelen == sizeof(AUTHORITY) - 1 && memcmp(AUTHORITY, name, namelen ) == 0 )
			{
				dstring uri_host{reinterpret_cast<const char*> ( value ), valuelen};
				stream_data->uri_host( uri_host );
			}
			else // Normal headers
			{
				dstring key{ reinterpret_cast<const char*>( name ), namelen};
				dstring val{ reinterpret_cast<const char*> ( value ), valuelen};
				stream_data->add_header( key, val );
			}
			break;
	}

	if ( nghttp2_session_want_write( s_this->session_data.get() ) )
		s_this->do_write();
	return 0;
}

void session::do_write()
{
	LOGTRACE("do_write");
	if(connector())
		connector()->do_write();
	else
	{
		LOGERROR("session http2 ", this, " connector already destroyed");
		// go on with handler work, so that nghttp2 can call the generate_cb
	}
}

void session::on_connector_nulled()
{
	LOGTRACE("on_connector_nulled");
// 	s->on_request_canceled(error_code_distruction)
	// Streams should be destroyed by nghttp2

// 	if( should_stop() )
// 		delete this;
// 	else
// 		for(auto &s: streams) s->on_request_canceled(error_code_distruction);
	if ( stream_counter == 0)
		delete this; // The session kills every stream
}

void session::finished_stream() noexcept
{
	--stream_counter;
	if ( stream_counter == 0 && connector() == nullptr )
		delete this;
}

int session::on_frame_recv_callback(nghttp2_session *session_, const nghttp2_frame *frame, void *user_data )
{
	session* s_this = static_cast<session*>( user_data );
	LOGTRACE( "on_frame_recv_callback Stream id: ", frame->hd.stream_id , " type ", frame->hd.type );

	int32_t stream_id = frame->hd.stream_id;
	void* stream_data_v = nghttp2_session_get_stream_user_data( s_this->session_data.get(), stream_id );
	stream* stream_data = static_cast<stream*>( stream_data_v );

	if ( stream_data != nullptr  )
	{
		if ( frame->hd.type == NGHTTP2_HEADERS )
		{
			if ( frame->headers.cat == NGHTTP2_HCAT_REQUEST )
				stream_data->on_request_header_complete();
			else
				LOGERROR("Strangeness in HTTP2 Headers");
		}

		// bitwise operator is not an error
		if ( frame->hd.flags & NGHTTP2_FLAG_END_STREAM )
			stream_data->on_request_finished(); // 1 stream, 1 message/response ← probably false
	}

	if ( nghttp2_session_want_write( s_this->session_data.get() ) )
		s_this->do_write();
	return 0;
}

int session::on_stream_close_callback( nghttp2_session* session_, int32_t stream_id, uint32_t error_code, void *user_data )
{
	LOGTRACE( "Stream close callback ", stream_id );
	session* s_this = static_cast<session*>( user_data );

	void* ud = nghttp2_session_get_stream_user_data( s_this->session_data.get(), stream_id );
	assert( ud );
	stream* stream_data = static_cast<stream*>( ud );

	stream_data->die();
	if ( nghttp2_session_want_write( s_this->session_data.get() ) )
		s_this->do_write();
	return 0;
}

bool session::should_stop() const noexcept
{
	return nghttp2_session_want_read( session_data.get() ) == 0 &&
		nghttp2_session_want_write( session_data.get() ) == 0;
}

int session::on_data_chunk_recv_callback(nghttp2_session *session_, uint8_t flags, int32_t stream_id,
	const uint8_t *data, size_t len, void *user_data )
{
	session* s_this = static_cast<session*>( user_data );
	stream* req = static_cast<stream*>( nghttp2_session_get_stream_user_data(session_, stream_id) );

	dstring body{ data, len }; /// @warning : we don't have ownership of this stuff - this should come out of input
	req->on_request_body( std::move(body) );

	/// @note return  NGHTTP2_ERR_PAUSE ; to pause input

	if ( nghttp2_session_want_write( s_this->session_data.get() ) )
		s_this->do_write();
	return 0;
}

int session::frame_send_callback (nghttp2_session *session_, const nghttp2_frame *frame, void *user_data )
{
// 	session* s_this = static_cast<session*>( user_data );
	std::int32_t stream_id = frame->hd.stream_id;
	LOGTRACE("frame_send_callback - Stream id: ", stream_id );

	//if (frame->hd.type != NGHTTP2_PUSH_PROMISE) ← only usage in his library
// 	void* stud = nghttp2_session_get_strea_muser_data( session_, stream_id );
//
// 	if ( stud != nullptr )
// 	{
// 		stream* stream_p = static_cast<stream*>( stud );
// // 		stream_p->flush();
//
// 	}
	return 0;
}

int session::frame_not_send_callback ( nghttp2_session *session_, const nghttp2_frame *frame,
	int lib_error_code, void *user_data )
{
	std::int32_t stream_id = frame->hd.stream_id;
	LOGERROR( "Frame not sent: ", nghttp2_strerror( lib_error_code ), " stream id: ", stream_id );

	// These get ignored in library's implementation
	if ( frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST )
		return 0;

	void* stud = nghttp2_session_get_stream_user_data( session_, stream_id );
	if ( stud != nullptr )
	{
		stream* stream_p = static_cast<stream*>( stud );
		stream_p->die();
	}
	// Issue RST_STREAM so that stream does not hang around.
	int rc = nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, stream_id, NGHTTP2_INTERNAL_ERROR);
	if ( rc != 0 ) // No mem or bug
		THROW( errors::failing_at_failing, rc );
//		LOGERROR( "RST STREAM ", stream_id, " ", nghttp2_strerror( rc ) );

	return 0;
}

bool session::on_write( dstring& ch )
{
	if ( connector() == nullptr ) return false;

	LOGTRACE("on_write");

	const uint8_t* data;
	int consumed = nghttp2_session_mem_send( session_data.get(), &data );

	if ( consumed < 0 ) // Memory exhausted!
		THROW ( errors::session_send_failure, consumed );

	if ( consumed >= 0 )
	{
		dstring tdata{data, static_cast<size_t>(consumed)};
		ch = std::move( tdata );
	}
	return true;
}

bool session::on_read(const char* data, size_t len)
{
	LOGTRACE("on_read");
	int rv = nghttp2_session_mem_recv( session_data.get(), reinterpret_cast<const uint8_t*>(data), len );

	if ( rv < 0 )
	{
		if ( rv == NGHTTP2_ERR_FLOODED || rv == NGHTTP2_ERR_CALLBACK_FAILURE )
			go_away();
		else // No mem or bad client magic ( unrecoverable error and bug )
			THROW( errors::session_recv_failure, rv );
	}
	assert( static_cast<unsigned int>(rv)== len );

	connector()->_rb.consume(data + len );
	if ( nghttp2_session_want_write( session_data.get() ) )
		do_write();
	return true;
}

}


