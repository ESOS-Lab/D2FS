import sys
import os

fname = sys.argv[2]
timegap = int(sys.argv[1])
#njobs = 4
#timegap = 2
last_start_t = 0
tmp_kiops_sum = 0
tmp_cnt = 0
is_first_line = True
with open(fname, 'r') as f:
    for l in f:
	if (is_first_line):
		is_first_line = False
		continue
        tmpl = l.strip().split()
		#print(tmpl)
        t = float(tmpl[0])
        kiops = float(tmpl[1])
        tmp_kiops_sum = tmp_kiops_sum + kiops
        tmp_cnt  = tmp_cnt + 1
        if (last_start_t + timegap <= t):
            print("{}\t{}".format(t, tmp_kiops_sum / tmp_cnt))
            tmp_cnt = 0
            tmp_kiops_sum = 0
            last_start_t = t
