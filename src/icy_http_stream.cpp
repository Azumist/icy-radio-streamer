#include "icy_http_stream.hpp"
#include "godot_cpp/classes/global_constants.hpp"
#include "godot_cpp/classes/http_client.hpp"
#include "godot_cpp/classes/tls_options.hpp"
#include "godot_cpp/variant/packed_byte_array.hpp"
#include "godot_cpp/variant/packed_string_array.hpp"
#include "godot_cpp/classes/time.hpp"
#include "godot_cpp/classes/os.hpp"
#include <mutex>

using namespace godot;

IcyHttpStream::IcyHttpStream() {
	client.instantiate();
	tls_options = TLSOptions::client();
	thread.instantiate();
}

IcyHttpStream::~IcyHttpStream() {
	cancel_request();
}

void IcyHttpStream::_bind_methods() {
	ClassDB::bind_method(D_METHOD("request", "url", "timeout", "parse_metadata", "custom_headers"), &IcyHttpStream::request, DEFVAL(5.0), DEFVAL(false), DEFVAL(PackedStringArray()));
	ClassDB::bind_method(D_METHOD("cancel_request"), &IcyHttpStream::cancel_request);
	ClassDB::bind_method(D_METHOD("is_requesting"), &IcyHttpStream::is_requesting);
	ClassDB::bind_method(D_METHOD("get_content_mime_type"), &IcyHttpStream::get_content_mime_type);
	ClassDB::bind_method(D_METHOD("get_response_headers"), &IcyHttpStream::get_response_headers);
	ClassDB::bind_method(D_METHOD("get_response_headers_as_dictionary"), &IcyHttpStream::get_response_headers_as_dictionary);
	ClassDB::bind_method(D_METHOD("get_downloaded_bytes"), &IcyHttpStream::get_downloaded_bytes);
	ClassDB::bind_method(D_METHOD("set_audio_decoder", "audio_decoder"), &IcyHttpStream::set_audio_decoder);

	ADD_SIGNAL(MethodInfo("connection_opened"));
	ADD_SIGNAL(MethodInfo("connection_closed", PropertyInfo(Variant::INT, "result")));
	ADD_SIGNAL(MethodInfo("metadata_received", PropertyInfo(Variant::STRING, "metadata")));

	BIND_ENUM_CONSTANT(RESULT_STREAM_FINISHED);
    BIND_ENUM_CONSTANT(RESULT_CANT_CONNECT);
    BIND_ENUM_CONSTANT(RESULT_CANT_RESOLVE);
    BIND_ENUM_CONSTANT(RESULT_CONNECTION_ERROR);
    BIND_ENUM_CONSTANT(RESULT_TIMEOUT);
    BIND_ENUM_CONSTANT(RESULT_REQUEST_FAILED);
    BIND_ENUM_CONSTANT(RESULT_REDIRECT_LIMIT_REACHED);
    BIND_ENUM_CONSTANT(RESULT_DISCONNECTED);
    BIND_ENUM_CONSTANT(RESULT_CANCELLED);
}

Error IcyHttpStream::_parse_url(const String &p_url) {
	use_tls = false;
	requesting = false;
	got_response = false;
	request_string = "";
	content_type = "";
	url = "";
	port = 0;
	redirections = 0;
	downloaded.set(0);

	String scheme;
	String fragment;
	String parsed_url = p_url;

	int frag_idx = parsed_url.find("#");
	if (frag_idx != -1) {
		fragment = parsed_url.substr(frag_idx + 1);
		parsed_url = parsed_url.substr(0, frag_idx);
	}

	int scheme_idx = parsed_url.find("://");
	if (scheme_idx != -1) {
		scheme = parsed_url.substr(0, scheme_idx + 3);
		parsed_url = parsed_url.substr(scheme_idx + 3);
	} else {
		return ERR_INVALID_PARAMETER;
	}

	int path_idx = parsed_url.find("/");
	if (path_idx != -1) {
		request_string = parsed_url.substr(path_idx);
		parsed_url = parsed_url.substr(0, path_idx);
	} else {
		request_string = "/";
	}

	int port_idx = parsed_url.rfind(":");
	if (port_idx != -1) {
		// check for IPv6 brackets to ensure a colon from inside an IPv6 address won't be detected
		int bracket_idx = parsed_url.rfind("]");
		if (bracket_idx == -1 || port_idx > bracket_idx) {
			port = parsed_url.substr(port_idx + 1).to_int();
			url = parsed_url.substr(0, port_idx);
		} else {
			url = parsed_url;
		}
	} else {
		url = parsed_url;
	}

	if (scheme == "https://") {
		use_tls = true;
	} else if (scheme != "http://") {
		return ERR_INVALID_PARAMETER;
	}

	if (port == 0) {
		port = use_tls ? 443 : 80;
	}

	return OK;
}

Error IcyHttpStream::_request() {
	return client->connect_to_host(url, port, use_tls ? tls_options : nullptr);
}

Error IcyHttpStream::request(const String &p_url, real_t p_timeout, bool p_parse_metadata, const PackedStringArray &p_custom_headers) {
	if (requesting) { return ERR_BUSY; }

	Error err = _parse_url(p_url);
	if (err != OK) { return err; }

	parse_icy_metadata = p_parse_metadata;
	timeout_msec = static_cast<uint64_t>(p_timeout * 1000.0);
	timeout_start_msec = Time::get_singleton()->get_ticks_msec();

	request_headers.clear();
	request_headers.push_back("Connection: close");

	if (p_parse_metadata) {
		request_headers.push_back("Icy-MetaData: 1");
	}
	request_headers.append_array(p_custom_headers);

	icy_metaint = 0;
	audio_bytes_until_meta = 0;
	meta_bytes_remaining = 0;
	meta_state = MetaState::AUDIO;
	metadata_buffer.clear();

	requesting = true;
	thread_done.clear();
	thread_request_quit.clear();
	result_emitted.clear();
	client->set_blocking_mode(true);

	thread->start(callable_mp(this, &IcyHttpStream::_thread_func));

	return OK;
}

void IcyHttpStream::cancel_request() {
	if (!is_requesting()) { return; }

	thread_request_quit.set();
	if (thread.is_valid() && thread->is_started()) {
		thread->wait_to_finish();
	}

	client->close();
	requesting = false;
	got_response = false;
	response_code = 0;

	if (!result_emitted.is_set()) {
		_defer_done(RESULT_CANCELLED);
	}
}

bool IcyHttpStream::is_requesting() const {
	return requesting;
}

String IcyHttpStream::get_content_mime_type() const {
	return content_type;
}

void IcyHttpStream::set_audio_decoder(const Ref<IcyAudioDecoder> p_decoder) {
	std::lock_guard<std::mutex> lock(decoder_mutex);
	audio_decoder = p_decoder;
}

PackedStringArray IcyHttpStream::get_response_headers() const {
	return response_headers;
}

int IcyHttpStream::get_downloaded_bytes() const {
	return downloaded.get();
}

Dictionary IcyHttpStream::get_response_headers_as_dictionary() const {
	Dictionary dict;

	for (int i = 0; i < response_headers.size(); ++i) {
		int sp = response_headers[i].find(":");
		if (sp == -1) { continue; }
		String key = response_headers[i].substr(0, sp).strip_edges();
		String value = response_headers[i].substr(sp + 1).strip_edges();
		dict[key] = value;
	}

	return dict;
}

void IcyHttpStream::_thread_func() {
	Error err = _request();

	if (err != OK) {
		_defer_done(RESULT_CANT_CONNECT);
	} else {
		while (!thread_request_quit.is_set()) {
			if (timeout_msec > 0) {
				uint64_t current_ticks = Time::get_singleton()->get_ticks_msec();
				if (current_ticks - timeout_start_msec > timeout_msec) {
					_defer_done(RESULT_TIMEOUT);
					break;
				}
			}

			bool exit = _update_connection();
			if (exit) { break; }
			OS::get_singleton()->delay_usec(1000);
		}
	}
}

bool IcyHttpStream::_update_connection() {
	HTTPClient::Status current_status = client->get_status();

	switch(current_status) {
		case HTTPClient::STATUS_DISCONNECTED: {
			_defer_done(RESULT_DISCONNECTED);
			return true;
		}
		case HTTPClient::STATUS_RESOLVING:
		case HTTPClient::STATUS_CONNECTING:
		case HTTPClient::STATUS_REQUESTING: {
			client->poll();
			return false;
		}
		case HTTPClient::STATUS_CANT_RESOLVE: {
			_defer_done(RESULT_CANT_RESOLVE);
			return true;
		}
		case HTTPClient::STATUS_CANT_CONNECT: {
			_defer_done(RESULT_CANT_CONNECT);
			return true;
		}
		case HTTPClient::STATUS_CONNECTED: {
			if (got_response) {
				// reached end of chunked transfer or standard stream (unlikely for icecast unless it disconnects)
				_defer_done(RESULT_STREAM_FINISHED);
				return true;
			}

			Error err = client->request(HTTPClient::METHOD_GET, request_string, request_headers);
			if (err != OK) {
				_defer_done(RESULT_CONNECTION_ERROR);
				return true;
			}

			got_response = true;
			return false;
		}
		case HTTPClient::STATUS_BODY: {
			if (got_response && response_code == 0) {
				bool ret_value;
				if (_handle_response(&ret_value)) {
					return ret_value;
				}
			}

			client->poll();
			if (client->get_status() != HTTPClient::STATUS_BODY) {
				return false;
			}

			PackedByteArray chunk = client->read_response_body_chunk();
			if (chunk.size() > 0) {
				downloaded.add(chunk.size());
				timeout_start_msec = Time::get_singleton()->get_ticks_msec();
				_process_chunk(chunk);
			}

			return false;
		}
		case HTTPClient::STATUS_CONNECTION_ERROR:
		case HTTPClient::STATUS_TLS_HANDSHAKE_ERROR: {
			_defer_done(RESULT_CONNECTION_ERROR);
			return true;
		}
	}

	return false;
}

bool IcyHttpStream::_handle_response(bool *ret_value) {
	if (!client->has_response()) {
		_defer_done(RESULT_REQUEST_FAILED);
		*ret_value = true;
		return true;
	}

	response_headers.clear();
	response_headers = client->get_response_headers();
	response_code = client->get_response_code();

	if (response_code == 301 || response_code == 302) {
		if (max_redirections >= 0 && redirections >= max_redirections) {
			_defer_done(RESULT_REDIRECT_LIMIT_REACHED);
			*ret_value = true;
			return true;
		}

		String new_request;
		for (int i = 0; i < response_headers.size(); ++i) {
			if (response_headers[i].to_lower().begins_with("location: ")) {
				new_request = response_headers[i].substr(10).strip_edges();
			}
		}

		if (!new_request.is_empty()) {
			client->close();
			int updated_redirs = redirections + 1;
			if (new_request.begins_with("http")) {
				_parse_url(new_request);
			} else {
				request_string = new_request;
			}

			Error err = _request();
			if (err == OK) {
				got_response = false;
				response_code = 0;
				downloaded.set(0);
				redirections = updated_redirs;
				*ret_value = false;
				return true;
			}
		}
	}

	bool found_content_type = false;
	bool found_icy_metaint = !parse_icy_metadata;

	for (int i = 0; i < response_headers.size(); ++i) {
		String lower_header = response_headers[i].to_lower();

		if (!found_content_type && lower_header.begins_with("content-type:")) {
			PackedStringArray parts = response_headers[i].split(":", false, 1);
			if (parts.size() > 1) {
				content_type = parts[1].strip_edges();
			}
			found_content_type = true;
		}
		else if (!found_icy_metaint && lower_header.begins_with("icy-metaint:")) {
			PackedStringArray parts = response_headers[i].split(":", false, 1);
			if (parts.size() > 1) {
				icy_metaint = parts[1].to_int();
				audio_bytes_until_meta = icy_metaint;
			}
			found_icy_metaint = true;
		}

		if (found_content_type && found_icy_metaint) { break; }
	}

	_defer_connected();

	return false;
}

void IcyHttpStream::_process_chunk(const PackedByteArray &p_chunk) {
	if (icy_metaint == 0 || !parse_icy_metadata) {
		_defer_data(p_chunk);
		return;
	}

	int offset = 0;
	int size = p_chunk.size();
	const uint8_t *rptr = p_chunk.ptr();

	while(offset < size) {
		if (meta_state == MetaState::AUDIO) {
			int bytes_to_read = MIN(audio_bytes_until_meta, size - offset);
			if (bytes_to_read > 0) {
				PackedByteArray audio_slice = p_chunk.slice(offset, offset + bytes_to_read);
				_defer_data(audio_slice);

				offset += bytes_to_read;
				audio_bytes_until_meta -= bytes_to_read;
			}

			if (audio_bytes_until_meta == 0) {
				meta_state = MetaState::LENGTH_BYTE;
			}
		}
		else if (meta_state == MetaState::LENGTH_BYTE) {
			int meta_length_multiplier = rptr[offset];
			meta_bytes_remaining = meta_length_multiplier * 16;
			++offset;

			if (meta_bytes_remaining > 0) {
				meta_state = MetaState::METADATA;
				metadata_buffer.clear();
			} else {
				meta_state = MetaState::AUDIO;
				audio_bytes_until_meta = icy_metaint;
			}
		}
		else if (meta_state == MetaState::METADATA) {
			int bytes_to_read = MIN(meta_bytes_remaining, size - offset);

			metadata_buffer.append_array(p_chunk.slice(offset, offset + bytes_to_read));

			offset += bytes_to_read;
			meta_bytes_remaining -= bytes_to_read;

			if (meta_bytes_remaining == 0) {
				String meta_str;
				meta_str.parse_utf8((const char*)metadata_buffer.ptr(), metadata_buffer.size());
				_defer_metadata(meta_str);

				meta_state = MetaState::AUDIO;
				audio_bytes_until_meta = icy_metaint;
			}
		}
	}
}

void IcyHttpStream::_defer_connected() {
	call_deferred("emit_signal", "connection_opened");
}

void IcyHttpStream::_defer_done(int p_status) {
	result_emitted.set();
	call_deferred("emit_signal", "connection_closed", p_status);
	thread_request_quit.set();
}

void IcyHttpStream::_defer_data(const PackedByteArray &p_data) {
	std::lock_guard<std::mutex> lock(decoder_mutex);

	if (audio_decoder.is_valid()) {
		audio_decoder->push_encoded_data(p_data);
	}
}

void IcyHttpStream::_defer_metadata(const String &p_meta) {
	// icecast pads metadata with null bytes, strip before emitting
	String clean_meta = p_meta.strip_edges().replace(String::chr(0), "");
	if (!clean_meta.is_empty()) {
		call_deferred("emit_signal", "metadata_received", clean_meta);
	}
}
