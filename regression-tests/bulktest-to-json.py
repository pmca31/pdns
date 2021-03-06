#!/usr/bin/env python
import glob, json

varnames = set()
statnames = set()
runs = list()

for fname in glob.glob('testresults-*.xml'):
	info = fname[12:-4].split('.')
	tag = info.pop(0)
	vars = dict(s.split(':') for s in info)
	vars['tag'] = tag
	varnames.update(vars.keys())
	stats=dict()
	for line in open(fname):
		if line.startswith('&lt;'):
			sname = line.split(';')[4][:-3]
			sval = line.split(';')[8][:-3]
			stats[sname]=sval
			statnames.add(sname)
	# print fname, vars, stats
	runs.append(dict(vars.items()+stats.items()))

# print varnames
# print statnames

print json.dumps(runs)
