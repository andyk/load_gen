#!/usr/bin/python
import sys
import fileinput
import operator
import math
import numpy
#import matplotlib
#matplotlib.use("PDF")
#import matplotlib.pyplot as pyplot

code_ct = {-1: 0, 1: 0, 2: 0, 3: 0, 4: 0, 5: 0 }

def count_ret( ret ):
  if ((ret < 100 or ret >= 600) and ret != -1):	
    print >> sys.stderr, "Unexpected HTTP return code: ", ret
    return
  code_ct[int(ret / 100)] += 1;

def percentile(list, p):
  assert(len(list) != 0 and p != 100 and p != 0)
  list = sorted(list)
  n = int(round(len(list)*p/100 + 0.5))
  print p,"%:", list[n - 1]
	
def main():
  SLA = 0.4
  req_start_count = 0
  req_end_count = 0

  start_delay = []
  conn_delay = []

  resp_times = []
  start_times = {}
  errors = {}

  first_time = -1
  skip_time = 0
  skip_id = []

  if len(sys.argv) > 2:
    print "Usage: " + sys.argv[0] + " [skip time] < trace"
    sys.exit(0)
  elif len(sys.argv) == 2:
    skip_time = float(sys.argv[1])

  for line in fileinput.input('-'):
    parts = line.rstrip().split(" ")

    if len(parts) < 3:
      print "Error parsing line:", line
      continue

    time = float(parts[0])
    type = parts[1]
    id = int(parts[2])

    if first_time == -1:
      first_time = time

    # Skip lines that start within the skip_time and skip lines with ids
    # we skipped before
    if time - first_time < skip_time:
      skip_id.append(id)
      continue
    if id in skip_id:
      continue
    
    if type == "start":
      req_start_count += 1
      start_times[id] = time
      start_delay.append(float(parts[3]))
    elif type == "conn":
      conn_delay.append(float(parts[3]))
    elif type == "end":
      req_end_count += 1
      ret_code = int(parts[3])
      sock_err = parts[4]
      resp_time = float(parts[5])

      if id not in start_times:
        print >> sys.stderr, "Id", id, "not found"
        sys.exit(1)
      start_time = start_times[id]

      if (ret_code != -1 and sock_err != "ok"):
        print >> sys.stderr, "Unexpected return code on line: ", line

      count_ret(ret_code)
      if ret_code != -1:
        resp_times.append((start_time,resp_time))
      else:
        errors[sock_err] = errors.setdefault(sock_err, 0) + 1

  if req_start_count != req_end_count:
    print
    print "Missing", req_start_count - req_end_count, "requests"
 
  print 
  if len(start_delay) > 0:
    print "Start time delay: max", numpy.max(start_delay), "min", numpy.min(start_delay), "mean", numpy.mean(start_delay), "std. dev.", numpy.std(start_delay)
  if len(conn_delay) > 0:  
    print "Connection delay: max", numpy.max(conn_delay), "min", numpy.min(conn_delay), "mean", numpy.mean(conn_delay), "std. dev.", numpy.std(conn_delay)

  print
  print "Response codes:"
  for k, v in code_ct.iteritems():
    if (k == -1):
      print "ERR: ", errors
    else:
      print str(k)+ "00: ", v

  print
  if (len(resp_times) > 0):
    time, resp = zip(*sorted(resp_times, key=operator.itemgetter(0)))

    # Group response times by the second the request started
    grouped_resp = {}
    for t, r in resp_times:
      k = math.floor(t);
      if k not in grouped_resp:
        grouped_resp[k] = []
      grouped_resp[k].append(r) 

    # Create reponse time stats for plotting
    f = open('./client_stat', 'w')
    print >>f, "time", "avg_resp_time", "std_resp_time", "min_resp_time", "max_resp_time", "successful"
    items = sorted(grouped_resp.items(), key=operator.itemgetter(0))
    p_times = []
    p_mean = []
    p_std = []
    for k, v in items:
      p_times.append(k)
      p_mean.append(numpy.mean(v))
      p_std.append(numpy.std(v))
      # Count the number of requests in this second
      successful = reduce(lambda sum, v: sum+1 if v < SLA else sum, v, 0)
      print >>f, k, numpy.mean(v), numpy.std(v), numpy.min(v), numpy.max(v), successful
    f.close()
    
    #pyplot.plot(p_times,p_mean,'b',label='avg resp time')
    #pyplot.plot(p_times,numpy.add((p_mean),(p_std)),'r',label='std dev resp time')
    #pyplot.savefig("resp.pdf")

    print "Reponse times:"
    print "Mean:", numpy.mean(resp)
    print "Std. Dev:", numpy.std(resp)
    print
    
    hist, buckets = numpy.histogram(resp, bins=10, range=(0,1))
    #pyplot.hist(resp, bins=20, range=(0,1))
    #pyplot.savefig("hist.pdf")

    print "Distribution: "
    for i in range(0, len(buckets)-1):
      print "[", buckets[i], ",", buckets[i+1], "] :", hist[i]
    print "> 1.0 :", len(resp) - sum(hist)

    print
    print "Percentiles:"
    percentile(resp, 99)
    percentile(resp, 99.9)
    percentile(resp, 99.99)
    percentile(resp, 99.999)

if __name__ == "__main__":
  main()
