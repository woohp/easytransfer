#!/usr/bin/env ruby
require 'optparse'
require 'net/http'
require 'tmpdir'

# global variables
$ip = nil
$port = 1234

# construct the full path given a uuid
# precondition: $ip and $port are initialized
def make_full_uri(uuid)
  "http://#{$ip}:#{$port}/#{uuid}"
end


def main()
  count = 1
  duration = 3600
  verbose = false
 
  opts = OptionParser.new do |opts|
    opts.banner = "Usage: share.rb [options] [file1, file2, ...]\n" +
      "\tif file is 'ls', then the currently shared list of files is displayed"

    opts.separator ""
    opts.separator "Specific options:"

    opts.on('-c', '--count COUNT', Integer, "Allow given number of total downloads") do |c|
      count = c || count
    end

    opts.on('-t', '--time TIME', Integer, "Expires after given amount of seconds") do |t|
      duration = t || duration
    end

    opts.on('-v', '--verbose', "Show extra output") do
      verbose = true
    end
    
    opts.on('-h', '--help', "Display this screen.") do
      puts opts
      exit
    end
  end
  opts.parse!(ARGV)
  # at this point, assume that ARGV only contains file names

  # get our external IP using whatismyip.com
  $ip = Net::HTTP.get(URI('http://automation.whatismyip.com/n09230945.asp'))
  
  # initialize the port by reading the settings file if possible
  settings_file = Dir.tmpdir() + "/.httpserver"
  if File.exists?(settings_file)
    $port = IO.read(settings_file).to_i || $port
  else
    puts "failed to read the settings file, defaulting to port #{port}"
  end

  for path in ARGV
    # if path is ls, then ask for for the current list of mappings by
    # using no additional uri string
    if path == 'ls'
      res = Net::HTTP.get(URI("http://localhost:#{$port}"))
      puts "Currently shared files:"
      for row in res.split("\n")
        comma_index = row.index(',')
        uuid = row[0, comma_index]
        path = row[comma_index+1, 0xffffffff]
        puts "#{path}\t==>\t#{make_full_uri(uuid)}"
      end
      next
    end
    
    # create the query string
    absolute_path = File.expand_path(path)
    uri = URI(URI.escape("http://localhost:#{$port}#{absolute_path}"))

    # send request and get response
    res = Net::HTTP.post_form(uri, :count => count, :time => duration)
    uuid = res.body.to_i

    puts make_full_uri(uuid)
  end
end


main()

