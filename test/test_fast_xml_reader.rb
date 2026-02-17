# frozen_string_literal: true

require 'minitest/autorun'
require 'stringio'
require 'tempfile'
require 'fast_xml_reader'

class TestFastXmlReader < Minitest::Test
  FIXTURE_DIR = File.expand_path('fixtures', __dir__)

  def reader_for(xml)
    FastXmlReader.new(StringIO.new(xml))
  end

  def collect_nodes(xml)
    r = reader_for(xml)
    nodes = []
    r.each { |n| nodes << { name: n.name, type: n.node_type, depth: n.depth, value: n.value } }
    nodes
  end

  # ── Initialization ──────────────────────────────────────────────────

  def test_new_with_file_path
    path = File.join(FIXTURE_DIR, 'sample.xml')
    r = FastXmlReader.new(path)
    assert_equal true, r.read
    r.close
  end

  def test_new_with_io
    r = reader_for('<root/>')
    assert_equal true, r.read
    r.close
  end

  def test_new_with_missing_file_raises
    assert_raises(Errno::ENOENT) { FastXmlReader.new('/nonexistent/path.xml') }
  end

  # ── Iteration ───────────────────────────────────────────────────────

  def test_read_returns_true_then_false
    r = reader_for('<a/>')
    assert_equal true, r.read
    assert_equal false, r.read
  end

  def test_each_yields_self
    r = reader_for('<a>text</a>')
    yielded = []
    r.each { |n| yielded << n }
    assert yielded.all? { |n| n.equal?(r) }
  end

  def test_each_returns_enumerator_without_block
    r = reader_for('<a/>')
    enum = r.each
    assert_kind_of Enumerator, enum
  end

  def test_each_enumerator_works
    r = reader_for('<a>hello</a>')
    names = r.each.map(&:name)
    assert_includes names, 'a'
  end

  # ── Node properties ─────────────────────────────────────────────────

  def test_name_returns_element_name
    r = reader_for('<item/>')
    r.read
    assert_equal 'item', r.name
  end

  def test_name_returns_nil_for_text_node
    r = reader_for('<a>text</a>')
    r.read # <a>
    r.read # text
    assert_equal FastXmlReader::TYPE_TEXT, r.node_type
    assert_nil r.name
  end

  def test_node_type_element
    r = reader_for('<a>text</a>')
    r.read
    assert_equal FastXmlReader::TYPE_ELEMENT, r.node_type
  end

  def test_node_type_text
    r = reader_for('<a>text</a>')
    r.read # <a>
    r.read # text
    assert_equal FastXmlReader::TYPE_TEXT, r.node_type
  end

  def test_node_type_end_element
    r = reader_for('<a>text</a>')
    r.read # <a>
    r.read # text
    r.read # </a>
    assert_equal FastXmlReader::TYPE_END_ELEMENT, r.node_type
  end

  def test_depth_tracking
    nodes = collect_nodes('<root><child><deep>x</deep></child></root>')
    depths = nodes.map { |n| n[:depth] }
    # root(0), child(1), deep(2), text(3), /deep(2), /child(1), /root(0)
    assert_equal [0, 1, 2, 3, 2, 1, 0], depths
  end

  def test_depth_self_closing
    nodes = collect_nodes('<root><br/></root>')
    # root(0), br(1)=empty, /root(0)
    depths = nodes.map { |n| n[:depth] }
    assert_equal [0, 1, 0], depths
  end

  def test_value_returns_text_content
    r = reader_for('<a>hello</a>')
    r.read # <a>
    r.read # text
    assert_equal 'hello', r.value
  end

  def test_value_returns_nil_for_elements
    r = reader_for('<a>text</a>')
    r.read # <a>
    assert_nil r.value
  end

  def test_attribute_returns_value
    r = reader_for('<item id="42" class="main"/>')
    r.read
    assert_equal '42', r.attribute('id')
    assert_equal 'main', r.attribute('class')
  end

  def test_attribute_returns_nil_if_missing
    r = reader_for('<item id="42"/>')
    r.read
    assert_nil r.attribute('missing')
  end

  def test_empty_element_self_closing
    r = reader_for('<br/>')
    r.read
    assert_equal true, r.empty_element?
    assert_equal true, r.self_closing?
  end

  def test_empty_element_collapsed
    r = reader_for('<x></x>')
    r.read
    assert_equal true, r.empty_element?
    assert_equal 'x', r.name
  end

  def test_non_empty_element
    r = reader_for('<a>text</a>')
    r.read
    assert_equal false, r.empty_element?
  end

  # ── Entity decoding ─────────────────────────────────────────────────

  def test_named_entity_amp
    r = reader_for('<a>one &amp; two</a>')
    r.read; r.read
    assert_equal 'one & two', r.value
  end

  def test_named_entity_lt
    r = reader_for('<a>&lt;tag&gt;</a>')
    r.read; r.read
    assert_equal '<tag>', r.value
  end

  def test_named_entity_quot_and_apos
    r = reader_for('<a>&quot;hello&apos;s&quot;</a>')
    r.read; r.read
    assert_equal %("hello's"), r.value
  end

  def test_numeric_decimal_entity
    r = reader_for('<a>&#65;&#66;</a>')
    r.read; r.read
    assert_equal 'AB', r.value
  end

  def test_numeric_hex_entity
    r = reader_for('<a>&#x41;&#x42;</a>')
    r.read; r.read
    assert_equal 'AB', r.value
  end

  def test_multibyte_numeric_entity
    r = reader_for('<a>&#x00E9;</a>')
    r.read; r.read
    assert_equal "\u00E9", r.value # é
  end

  def test_entity_in_attribute
    r = reader_for('<a val="one &amp; two"/>')
    r.read
    assert_equal 'one & two', r.attribute('val')
  end

  def test_unknown_entity_passed_through
    r = reader_for('<a>&unknown;</a>')
    r.read; r.read
    assert_equal '&unknown;', r.value
  end

  def test_text_without_entities_no_decode
    r = reader_for('<a>plain text</a>')
    r.read; r.read
    assert_equal 'plain text', r.value
  end

  # ── Namespace handling ──────────────────────────────────────────────

  def test_namespace_prefix_stripped_from_element
    r = reader_for('<ns:item/>')
    r.read
    assert_equal 'item', r.name
  end

  def test_namespace_prefix_stripped_from_end_element
    r = reader_for('<ns:item>text</ns:item>')
    r.read # <ns:item>
    r.read # text
    r.read # </ns:item>
    assert_equal 'item', r.name
    assert_equal FastXmlReader::TYPE_END_ELEMENT, r.node_type
  end

  def test_xmlns_attributes_filtered
    r = reader_for('<root xmlns:ns="http://example.com" id="1"/>')
    r.read
    assert_nil r.attribute('xmlns:ns')
    assert_equal '1', r.attribute('id')
  end

  def test_xmlns_default_filtered
    r = reader_for('<root xmlns="http://example.com" id="1"/>')
    r.read
    assert_nil r.attribute('xmlns')
    assert_equal '1', r.attribute('id')
  end

  # ── Skipped content ─────────────────────────────────────────────────

  def test_comments_skipped
    nodes = collect_nodes('<a><!-- comment -->text</a>')
    types = nodes.map { |n| n[:type] }
    refute_includes types, nil
    text_nodes = nodes.select { |n| n[:type] == FastXmlReader::TYPE_TEXT }
    assert_equal 1, text_nodes.size
    assert_equal 'text', text_nodes.first[:value]
  end

  def test_processing_instructions_skipped
    nodes = collect_nodes('<?xml version="1.0"?><root>hello</root>')
    names = nodes.map { |n| n[:name] }.compact
    refute_includes names, 'xml'
    assert_includes names, 'root'
  end

  def test_cdata_skipped
    nodes = collect_nodes('<a><![CDATA[some data]]>text</a>')
    text_values = nodes.select { |n| n[:type] == FastXmlReader::TYPE_TEXT }.map { |n| n[:value] }
    assert_equal ['text'], text_values
  end

  def test_doctype_skipped
    nodes = collect_nodes('<!DOCTYPE html><root>hello</root>')
    names = nodes.map { |n| n[:name] }.compact
    assert_includes names, 'root'
  end

  def test_blank_text_nodes_skipped
    nodes = collect_nodes("<root>\n  <child/>\n</root>")
    types = nodes.map { |n| n[:type] }
    refute_includes types, FastXmlReader::TYPE_TEXT
  end

  # ── Resource management ─────────────────────────────────────────────

  def test_close_releases_resources
    r = reader_for('<a/>')
    r.read
    r.close
    # After close, read should return false (no data)
    assert_equal false, r.read
  end

  def test_close_idempotent
    r = reader_for('<a/>')
    r.close
    r.close # should not raise
  end

  # ── Constants ───────────────────────────────────────────────────────

  def test_type_element_constant
    assert_equal 1, FastXmlReader::TYPE_ELEMENT
  end

  def test_type_text_constant
    assert_equal 3, FastXmlReader::TYPE_TEXT
  end

  def test_type_end_element_constant
    assert_equal 15, FastXmlReader::TYPE_END_ELEMENT
  end

  # ── Integration: full document traversal ────────────────────────────

  def test_full_traversal
    xml = '<root><a id="1">hello</a><b/></root>'
    nodes = collect_nodes(xml)

    assert_equal 'root',  nodes[0][:name]
    assert_equal FastXmlReader::TYPE_ELEMENT, nodes[0][:type]

    assert_equal 'a',     nodes[1][:name]
    assert_equal FastXmlReader::TYPE_ELEMENT, nodes[1][:type]

    assert_nil            nodes[2][:name]
    assert_equal FastXmlReader::TYPE_TEXT, nodes[2][:type]
    assert_equal 'hello', nodes[2][:value]

    assert_equal 'a',     nodes[3][:name]
    assert_equal FastXmlReader::TYPE_END_ELEMENT, nodes[3][:type]

    assert_equal 'b',     nodes[4][:name]
    assert_equal true,    reader_for(xml).tap { |r| 5.times { r.read } }.empty_element?

    assert_equal 'root',  nodes[5][:name]
    assert_equal FastXmlReader::TYPE_END_ELEMENT, nodes[5][:type]
  end

  def test_file_io_matches_path_results
    path = File.join(FIXTURE_DIR, 'sample.xml')
    path_nodes = []
    r = FastXmlReader.new(path)
    r.each { |n| path_nodes << { name: n.name, type: n.node_type, depth: n.depth, value: n.value } }
    r.close

    io_nodes = []
    File.open(path) do |f|
      r = FastXmlReader.new(f)
      r.each { |n| io_nodes << { name: n.name, type: n.node_type, depth: n.depth, value: n.value } }
      r.close
    end

    assert_equal path_nodes, io_nodes
  end

  def test_mmap_fixture_traversal
    path = File.join(FIXTURE_DIR, 'sample.xml')
    r = FastXmlReader.new(path)
    nodes = []
    r.each { |n| nodes << n.name }
    r.close
    assert_includes nodes, 'root'
    assert_includes nodes, 'item'
  end

  def test_attribute_single_quotes
    r = reader_for("<a val='single'/>")
    r.read
    assert_equal 'single', r.attribute('val')
  end

  def test_multiple_namespaces
    xml = '<ns1:a><ns2:b>text</ns2:b></ns1:a>'
    nodes = collect_nodes(xml)
    names = nodes.map { |n| n[:name] }.compact
    assert_equal %w[a b b a], names
  end

  def test_value_encoding_is_utf8
    r = reader_for('<a>hello</a>')
    r.read; r.read
    assert_equal Encoding::UTF_8, r.value.encoding
  end

  def test_name_encoding_is_utf8
    r = reader_for('<hello/>')
    r.read
    assert_equal Encoding::UTF_8, r.name.encoding
  end

  def test_interned_names_are_frozen
    r = reader_for('<a><a/></a>')
    r.read
    name1 = r.name
    r.read
    name2 = r.name
    assert name1.frozen?
    assert name2.frozen?
    assert_equal name1, name2
  end
end
