#include "handler_factory.h"
#include "handler_http1.h"
#include "handler_http2.h"
#include "../http2/session.h"
#include "../connector.h"
#include "../utils/log_wrapper.h"
#include "../chain_of_responsibility/node_interface.h"
#include "../chain_of_responsibility/error_code.h"
#include "../chain_of_responsibility/chain_of_responsibility.h"
#include "../requests_manager/method_filter.h"
#include "../requests_manager/error_producer.h"
#include "../requests_manager/client_wrapper.h"
#include "../requests_manager/date_setter.h"
#include "../requests_manager/sys_filter.h"
#include "../requests_manager/interreg_filter.h"
#include "../requests_manager/error_file_provider.h"
#include "../requests_manager/header_filter.h"
#include "../requests_manager/franco_host.h"
#include "../requests_manager/request_stats.h"
#include "../requests_manager/id_header_adder.h"
#include "../requests_manager/configurable_header_filter.h"
#include "../requests_manager/gzip_filter.h"
#include "../requests_manager/cache_manager/cache_manager.h"
#include "../requests_manager/cache_cleaner.h"
#include "../dummy_node.h"
#include "../utils/likely.h"
#include "../configuration/configuration_wrapper.h"

#include <string>

using namespace std;

namespace server
{

const static string alpn_protos_default[]
{
	"\x2h2",
	"\x5h2-16",
	"\x5h2-14",
	"\x8http/1.1",
	"\x8http/1.0"
};
const static string alpn_protos_legacy[]
{\
	"\x8http/1.1",
	"\x8http/1.0"
};

const static unsigned char npn_protos[]
{
	2,'h','2',
	5,'h','2','-','1','6',
	5,'h','2','-','1','4',
	8,'h','t','t','p','/','1','.','1',
	8,'h','t','t','p','/','1','.','0'
};
const static unsigned char npn_protos_legacy[]
{
	8,'h','t','t','p','/','1','.','1',
	8,'h','t','t','p','/','1','.','0'
};

using alpn_cb = int (*)
	(ssl_st*, const unsigned char**, unsigned char*, const unsigned char*, unsigned int, void*);

alpn_cb alpn_select_cb = [](ssl_st *ctx, const unsigned char** out, unsigned char* outlen,
		const unsigned char* in, unsigned int inlen, void *data)
{
	//string local_alpn_protos[5] = (service_locator::configuration().http2_is_disabled()) ?  alpn_protos_legacy : alpn_protos_default;
	if(service::locator::configuration().http2_is_disabled())
	{
		for( auto& proto : alpn_protos_legacy )
			if(SSL_select_next_proto((unsigned char**)out, outlen, (const unsigned char*)proto.c_str(), proto.size()-1, in, inlen) == OPENSSL_NPN_NEGOTIATED)
				return SSL_TLSEXT_ERR_OK;
	}
	else
	{
		for( auto& proto : alpn_protos_default )
			if(SSL_select_next_proto((unsigned char**)out, outlen, (const unsigned char*)proto.c_str(), proto.size()-1, in, inlen) == OPENSSL_NPN_NEGOTIATED)
				return SSL_TLSEXT_ERR_OK;
	}
	return SSL_TLSEXT_ERR_NOACK;
};

using npn_cb = int (*) (SSL *ssl, const unsigned char** out, unsigned int* outlen, void *arg);
npn_cb npn_adv_cb = [](ssl_st *ctx, const unsigned char** out, unsigned int* outlen, void *arg)
{
	*out = (service::locator::configuration().http2_is_disabled()) ? npn_protos_legacy : npn_protos;
	*outlen = sizeof(npn_protos);
	return SSL_TLSEXT_ERR_OK;
};

void handler_factory::register_protocol_selection_callbacks(SSL_CTX* ctx)
{
	SSL_CTX_set_next_protos_advertised_cb(ctx, npn_adv_cb, nullptr);
	SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, nullptr);
}

handler_interface* handler_factory::negotiate_handler(const SSL *ssl) const noexcept
{
	const unsigned char* proto{nullptr};
	unsigned int len{0};

	//ALPN
	SSL_get0_alpn_selected(ssl, &proto, &len);
	if( !len )
		//NPN
		SSL_get0_next_proto_negotiated(ssl, &proto, &len);


	handler_type type{ht_h1};
	if( len )
	{
		LOGDEBUG("Protocol negotiated: ", std::string(reinterpret_cast<const char*>(proto), len));
		switch(len)
		{
			case 2:
			case 3:
			case 5:
				type = ht_h2;
				break;
		}
	}

	http::proto_version version =
			(type == ht_h2) ? http::proto_version::HTTP20 : ((proto == nullptr ||   proto[len-1] == '0') ? http::proto_version::HTTP10 : http::proto_version::HTTP11);


	return build_handler( type , version );
}

boost::asio::ip::address handler_interface::find_origin() const
{
	return _connector->origin();
}

handler_interface* handler_factory::build_handler(handler_type type, http::proto_version proto) const noexcept
{
	switch(type)
	{
		case ht_h1:
		{
			LOGDEBUG("HTTP1 selected");
			return new handler_http1( proto );
		}
		case ht_h2:
			if ( service::locator::configuration().http2_ng() )
			{
				LOGTRACE("HTTP2 NG selected");
				return new http2::session();
			}
			else
			{
				LOGTRACE("HTTP2 old selected");
				return new handler_http2();
			}
		case ht_unknown: return nullptr;
	}
	return nullptr;
}

void handler_interface::initialize_callbacks(node_interface &cor)
{
	header_callback hcb = [this](http::http_response&& headers){ /*on_header(move(headers)); */ };
	body_callback bcb = [this](dstring&& chunk){ /* on_body(std::move(chunk)); */};
	trailer_callback tcb = [this](dstring&& k, dstring&& v){ /* on_trailer(move(k),move(v)); */};
	end_of_message_callback eomcb = [this](){on_eom();};
	error_callback ecb = [this](const errors::error_code& ec) { on_error( ec.code() ); };
	response_continue_callback rccb = [this](){ /*on_response_continue(); */};

    cor.initialize_callbacks(hcb, bcb, tcb, eomcb, ecb, rccb);
}

void handler_interface::connector( connector_interface * conn )
{
	LOGTRACE("handler_interface::connector ", conn );
	_connector = conn;
	if ( ! _connector )
		on_connector_nulled();
}

std::function<std::unique_ptr<node_interface>()> handler_interface::make_chain = []()
{
	return make_unique_chain<node_interface,
		nodes::error_producer, //check efa; node per se seems ok
		nodes::request_stats,
		nodes::date_setter,
		nodes::header_filter,
		nodes::method_filter,
		nodes::franco_host,
		nodes::sys_filter,
		nodes::cache_cleaner,
		nodes::interreg_filter,
		nodes::error_file_provider,
		nodes::id_header_adder,
		nodes::configurable_header_filter,
		nodes::cache_manager,
		nodes::gzip_filter,
		nodes::client_wrapper>();
};
	

void handler_interface::chain_initializer( std::function<std::unique_ptr<node_interface>()> newfunc )
{
	make_chain = newfunc;
}

} //namespace
