#!/usr/bin/python
"""
Converts a wikipedia trace to the format expected by load_gen.
Filters out special pages and non-english pages.
"""
import sys
import os
import time 

def containsAny(str, set):
	"""Check whether 'str' contains ANY of the chars in 'set'"""
	return 1 in [c in str for c in set]

if len(sys.argv) < 3:
	print "Usage: ./convert_wiki_trace.py <trace file> <output>"
	sys.exit(0)

url_prefix = "http://en.wikipedia.org"
drop_urls = ["?search=", "&search=", "wiki/Special:Search", "w/query.php", "wiki/Talk:", "User+talk", "User_talk", "wiki/Special:AutoLogin", "Special:UserLogin", "User:", "Talk:", "&diff=", "&action=rollback", "Special:Watchlist", "w/api.php"]

f = open(sys.argv[1])
o = open("/tmp/wiki_parse_tmp", "w")

for line in f:
	parts = line.rstrip().split(" ")
	if len(parts) != 4:
		continue
	if parts[3] != "-":
		continue
	if not parts[2].startswith(url_prefix):
		continue

	#1196494711.635
	timestamp = parts[1]
	path = parts[2][len(url_prefix):]
	path = path.replace("%2F", "/")
	path = path.replace("%20", " ")
	path = path.replace("&amp;", "&")
	path = path.replace("%3A", ":")
	
	if containsAny(path, drop_urls):
		continue

	print >>o, timestamp, path 
f.close()
o.close()
command = "sort -n -o " + sys.argv[2] + " /tmp/wiki_parse_tmp"
os.system(command)
