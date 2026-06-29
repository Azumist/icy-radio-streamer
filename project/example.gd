extends Node

var icy_http: IcyHttpStream;

func _ready() -> void:
	var request_headers: PackedStringArray = [
		"User-Agent: Godot/4.5"
	];
	
	icy_http = IcyHttpStream.new()
	
	icy_http.connection_opened.connect(func():
		var response_headers_dict := icy_http.get_response_headers_as_dictionary()
		var content_mime_type := icy_http.get_content_mime_type()
		
		print("response headers: ", response_headers_dict)
		print("mime: ", content_mime_type)
	)
	
	icy_http.connection_closed.connect(func(result: IcyHttpStream.Result):
		print("Connection closed: ", result)
	)
	
	icy_http.metadata_received.connect(func(metadata: String):
		print("Metadata: ", metadata)
	)
	
	icy_http.request("http://puma.streemlion.com:1220/stream", 5.0, true, request_headers)
