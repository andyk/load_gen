#!/usr/bin/python
"""
Generates a synthetic trace file to feed to the load generator. Computes request
arrival times from a rate function.
"""

import sys
import math
import random

window = 0.001

class SineWave:
	def __init__(self, period, min, max):
		self.period = period
		self.min = min
		self.max = max
	def eval(self, t):
		a = (self.max - self.min) / 2
		b = (2*math.pi) / self.period
		c = a + self.min
		return a * math.sin(b * t) + c
	def range(self):
		return self.period

class Line:
	def __init__(self, t1, y1, t2, y2):
		self.t1 = t1
		self.y1 = y1
		self.t2 = t2
		self.y2 = y2
	def eval(self, t):
		if t <= self.t1 or t > self.t2:
			return 0
		return ((self.y2 - self.y1)*(t - self.t1)) / (self.t2 - self.t1) + self.y1
	def range(self):
		return self.t2

# Helper function for making a sequence of lines
# args are tuples of (delta_x, y)
def Lines(start_y, *args):
  ret = []
  lx = 0
  ly = start_y
  for (dx, y) in args:
    #print >>sys.stderr, "Line: ", lx, ly, " - ", lx+dx, y
    ret.append( Line(lx, ly, lx+dx, y) )
    lx += dx
    ly = y

  return ret

class Noise:
  def __init__(self, obj):
    self.obj = obj
    self.noise = []
    amp = 8
    period = 1.1
    for i in range(0, 60):
      a = random.uniform(0, amp)
      self.noise.append(SineWave(random.uniform(0.0001, period), -a, a))

  def eval(self, t):
    noise = 0
    for n in self.noise:
      noise += n.eval(t)

    if t > 2: 
      return max(0, self.obj.eval(t) + noise )
    else:
      return self.obj.eval(t)

  def range(self):
    return self.obj.range()

def rate(t, input):
	sum = 0
	for i in input:
		sum += i.eval(t)
	return sum

def usage():
	print 'python synth_trace.py [type] [-u url | -f url_file]'
	print '\ttype: "sin" for sine wave based trace'
	print '\ttype: "trap" for trapazoid based trace'
	print '\t-u url: the url to request (e.g. /wiki/Berkeley)'
	print '\t-f url_file: a file with a list of urls to request'
	sys.exit(1)


# Static configuration of rate functions

sine_input = [ SineWave(10*60, 0, 5*10), #10min, max=10conn/sec/backend
               SineWave(0.2, 0, 0.5) ] #plus noise

node_perf = 20

start_n = 1.5 * node_perf
max_n = 14 * node_perf
stop_n = 3.5 * node_perf
flat_t = 50
slope = 1.5
trap_input = Lines(start_n, 
                   (flat_t, start_n),
                   ((max_n - start_n) / slope, max_n),
                   (flat_t, max_n),
                   ((max_n - stop_n) / slope, stop_n),
                   (flat_t, stop_n))

#trap_input = [ Line(0, start_n, 30, start_n),
#               Line(30, start_n, 120, max_n),
#               Line(120, max_n, 150, max_n),
#               Line(150, max_n, 210, stop_n),
#               Line(210, stop_n, 240, stop_n) ]
#

ramp_input = Lines(0, (60, 10*node_perf), (10, 10*node_perf), (60, 0))

noisy_sine = [ Noise(SineWave(200, -8*20, 8*20)) ]

def main():
	if len(sys.argv) != 4:
		usage()

	if sys.argv[1] == 'sin':
		func = sine_input
	elif sys.argv[1] == 'trap':
		func = trap_input
	elif sys.argv[1] == 'ramp':
		func = ramp_input
	elif sys.argv[1] == 'noisy':
		func = noisy_sine
	else:
		print 'Invalid option', sys.argv[1]
		sys.exit(1)

	duration = max([x.range() for x in func])
	if sys.argv[1] == 'noisy':
		duration = duration / 2

	if sys.argv[2] == '-u':
		urls = [sys.argv[3]]
	elif sys.argv[2] == '-f':
		urls = open(sys.argv[3], 'r').readlines()
	else:
		print 'Invalid option:', sys.argv[2]
		sys.exit(1)

	# Integrate rate to get request times
	time = 0
	integral = 0
	req = 0 
	while time < duration:
		integral += window * rate(time, func)
		time += window
		if integral >= req+1:
			print '%.3f %s' % (time, urls[req % len(urls)].rstrip())
			req += 1

if __name__ == '__main__':
	random.seed()
	main()
