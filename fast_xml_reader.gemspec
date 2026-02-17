# frozen_string_literal: true

Gem::Specification.new do |s|
  s.name        = 'fast_xml_reader'
  s.version     = '1.1.0'
  s.summary     = 'Fast, mmap-based XML pull reader for Ruby'
  s.description = 'Lightweight XML pull reader using mmap and zero-copy scanning ' \
                  'with C-level name interning. API compatible with Nokogiri::XML::Reader.'
  s.authors     = ['Felix Buenemann']
  s.license     = 'MIT'
  s.homepage    = 'https://github.com/felixbuenemann/fast_xml_reader'
  s.files       = Dir['lib/**/*.rb', 'ext/**/*.{c,rb}', 'README.md', 'LICENSE']
  s.extensions  = ['ext/fast_xml_reader/extconf.rb']
  s.required_ruby_version = '>= 2.1'
  s.require_paths = ['lib']
end
