# frozen_string_literal: true
require 'mkmf'

$CFLAGS << ' -std=c99 -O3 -Wall -Wextra -Wno-unused-parameter'

have_func('rb_enc_interned_str', 'ruby/encoding.h') # Ruby 3.0+

create_makefile('fast_xml_reader/fast_xml_reader')
