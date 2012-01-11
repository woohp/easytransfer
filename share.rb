#!/usr/bin/env ruby
require 'net/http'
require 'tmpdir'

def main()
  if ARGV.size == 0
    puts "usage: share [-c count] [-t duration] path_to_file"
    return
  end

  path = ARGV[0]
  count = 1
  duration = 3600
  for i in [0, 2]
    if ARGV[i] == '-c'
      count = ARGV[i+1]
      path = ARGV[i+2]
    elsif ARGV[i] == '-t'
      duration= ARGV[i+1]
      path = ARGV[i+2]
    end
  end

  ip = Net::HTTP.get(URI('http://automation.whatismyip.com/n09230945.asp'))
  port = 1235

  settings_file = Dir.tmpdir() + "/.httpserver"
  if File.exists?(settings_file)
    port = IO.read(settings_file)
  else
    puts "failed to read the settings file, defaulting to port #{port}"
  end

  absolute_path = File.expand_path(path)
  uri = URI(URI.escape("http://localhost:#{port}#{absolute_path}"))

  res = Net::HTTP.post_form(uri, :count => count, :time => duration)
  uuid = res.body.to_i

  puts "http://#{ip}:#{port}/#{uuid}"
end


main()
