#!/usr/bin/env ruby
require 'net/http'

def main()
  if ARGV.size != 1
    puts "usage: share path_to_file"
    return
  end

  ip = Net::HTTP.get(URI('http://automation.whatismyip.com/n09230945.asp'))

  absolute_path = File.expand_path(ARGV[0])
  uri = URI("http://localhost:1234#{absolute_path}")

  res = Net::HTTP.post_form(uri, '' => '')
  uuid = res.body.to_i

  puts "http://#{ip}:8080/#{uuid}"
end


main()
