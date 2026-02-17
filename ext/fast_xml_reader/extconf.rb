# frozen_string_literal: true
require 'mkmf'

$CFLAGS << ' -O3 -Wall -Wextra -Wno-unused-parameter'

create_makefile('fast_xml_reader/fast_xml_reader')
