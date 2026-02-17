# FastXmlReader

[![Test](https://github.com/felixbuenemann/ruby-fast-xml-reader/actions/workflows/test.yml/badge.svg)](https://github.com/felixbuenemann/ruby-fast-xml-reader/actions/workflows/test.yml)

Fast, lightweight XML pull reader for Ruby. Uses mmap and zero-copy scanning with C-level name interning for high-performance XML processing.

API compatible with Nokogiri::XML::Reader, but with no dependencies.

## Installation

```ruby
gem 'fast_xml_reader', git: 'https://github.com/felixbuenemann/fast_xml_reader'
```

## Usage

```ruby
require 'fast_xml_reader'

reader = FastXmlReader.new("path/to/file.xml")

reader.each do |node|
  case node.node_type
  when FastXmlReader::TYPE_ELEMENT
    puts "#{" " * node.depth}<#{node.name}>"
    value = node.attribute("id")
  when FastXmlReader::TYPE_TEXT
    puts "#{" " * node.depth}#{node.value}"
  when FastXmlReader::TYPE_END_ELEMENT
    puts "#{" " * node.depth}</#{node.name}>"
  end
end
```

You can also pass an IO object instead of a file path:

```ruby
File.open("file.xml") do |io|
  reader = FastXmlReader.new(io)
  reader.each { |node| ... }
end
```

## API

### `FastXmlReader.new(path_or_io)`

Creates a new reader. When given a file path (String), the file is memory-mapped for zero-copy access. When given an IO object, the content is read into a buffer.

### Node methods

| Method | Description |
|---|---|
| `node_type` | `TYPE_ELEMENT` (1), `TYPE_TEXT` (3), or `TYPE_END_ELEMENT` (15) |
| `name` | Element name (namespace prefix stripped, frozen/interned) |
| `depth` | Current tree depth |
| `value` | Text content (with XML entity decoding) |
| `attribute(name)` | Get attribute value by name |
| `empty_element?` | True if self-closing (`<br/>`) or empty (`<br></br>`) |
| `self_closing?` | Alias for `empty_element?` |

### Iteration

| Method | Description |
|---|---|
| `read` | Advance to next node, returns `true`/`false` |
| `each` | Yield each node (returns Enumerator if no block) |
| `close` | Release mmap/buffer early |

## Features

- **mmap** for file paths, buffered IO for streams
- **Zero-copy scanning** — points directly into the mmap buffer
- **Name interning** via FNV-1a hash table (512 entries) for fast string dedup
- **XML entity decoding** — `&amp;` `&lt;` `&gt;` `&quot;` `&apos;` and numeric (`&#123;` `&#x1A;`)
- **Namespace stripping** — `ns:element` is reported as `element`
- **Empty element collapsing** — `<x></x>` is treated as `<x/>`
- **Skips** comments, CDATA, DOCTYPE, and processing instructions

## License

MIT
