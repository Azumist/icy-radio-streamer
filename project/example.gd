extends Node

var icy_http: IcyHttpStream
var icy_audio_decoder: IcyAudioDecoder

@export var radio_player: AudioStreamPlayer

func _ready() -> void:
	var request_headers: PackedStringArray = [
		"User-Agent: Godot/4.5"
	];
	
	icy_http = IcyHttpStream.new()
	
	# --- connection lambda ---
	icy_http.connection_opened.connect(func():
		var response_headers_dict := icy_http.get_response_headers_as_dictionary()
		var content_mime_type := icy_http.get_content_mime_type()
		
		# 1. create appropriate decoders for the stream (currently only one)
		match content_mime_type:
			"audio/mpeg":
				icy_audio_decoder = IcyMp3AudioDecoder.new()
				
				# check for bitrate info - if there isn't one, decoder will treat it like the standard 128kbps mp3
				if response_headers_dict.has("icy-br"):
					icy_audio_decoder.set_bitrate_hint(int(response_headers_dict["icy-br"]))
				
				icy_audio_decoder.stream_info_ready.connect(func():
					# radio_player stream MUST be of type AudioStreamGenerator !!!
					var stream: AudioStreamGenerator = radio_player.stream
					
					# calculate appropriate sample rate and buffer size for the decoder
					# sample rate could also be read from response headers, but info directly from the codec is more reliable
					var rate: int = icy_audio_decoder.get_stream_sample_rate()
					stream.mix_rate = rate
					print("mix rate: %d Hz" % rate)
					
					var max_frames := int(stream.mix_rate * stream.buffer_length)
					var frames := int(stream.mix_rate / 2)
					icy_audio_decoder.set_buffer_thresholds(frames, max_frames)
				)
			_:
				push_error("Unsupported format: " + content_mime_type)
				icy_http.cancel_request()
				
		# 2. decoder is created, patch it thorugh the IcyHttpStream so we can play the audio now
		if icy_audio_decoder:
			icy_audio_decoder.buffering_started.connect(func():
				print("Buffering started (underrun detected)")
				radio_player.stream_paused = true
			)
			
			icy_audio_decoder.buffering_finished.connect(func():
				print("Buffering complete. Unpausing playback.")
				radio_player.stream_paused = false
			)
			
			# 3. start the playback and pause so we could grab the reference. 
			# buffering signals will unpause the playback when enough data is accumulated.
			radio_player.play()
			radio_player.stream_paused = true
			
			icy_audio_decoder.set_playback(radio_player.get_stream_playback())
			icy_http.set_audio_decoder(icy_audio_decoder)
	)
	# --- end of connection lambda ---
	
	icy_http.connection_closed.connect(func(result: IcyHttpStream.Result):
		print("Connection closed: ", result)
	)
	
	icy_http.metadata_received.connect(func(metadata: String):
		print("Metadata: ", metadata)
	)
	
	icy_http.request("http://puma.streemlion.com:1220/stream", 5.0, true, request_headers)

func _process(_delta: float) -> void:
	# decoder must be pushed very often to play back accummulated data and to make room for new one in the ring buffer
	# it doesn't really matter if it's called in the _process or _physics_process, 
	# the buffer will adjust the size of read chunks to the frequency of calls.
	# TODO: check if it's viable to call it via timer
	if icy_audio_decoder:
		icy_audio_decoder.process_audio()
