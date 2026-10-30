# frozen_string_literal: true

source 'https://rubygems.org'

gemspec

gem 'minitest', '~> 5.0'
gem 'rake'
# rake-compiler 1.3.2 uses syntax that needs Ruby 2.3, but still declares Ruby >= 1.8.7
if RUBY_VERSION < '2.3'
  gem 'rake-compiler', '~> 1.0', '< 1.3.2'
else
  gem 'rake-compiler', '~> 1.0'
end
