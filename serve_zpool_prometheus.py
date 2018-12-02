#!/usr/bin/env python
"""
Simple example of serving zpool_prometheus metrics using Flask

Note: if for some reason the zpool_prometheus program hangs, then
the results are undefined. When planning for production, expect
that the worst case is zpool_prometheus hangs in the kernel and is
not killable.

Prerequisites:
 + zpool_prometheus executable in $PATH
 + Flask is installed in the python libraries (eg pip install Flask)

To run, for testing:
  FLASK_APP=serve_zpool_prometheus.py python -m flask run

To test:
  curl localhost:5000

Pro tip: use a real WSGI server for production with proper security policies.

For the impatient: by default, flask binds only to localhost port 5000. To
allow access from elsewhere or change port, use the --host and --port options.
You can also set the FLASK_APP as an environment variable:
  export FLASK_APP=serve_zpool_prometheus.py
  python -m flask run --host=0.0.0.0 --port=5000

See Flask docs for more information on options

Copyright 2018 Richard Elling

The MIT License (MIT)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

"""
from subprocess import check_output
from flask import Flask, abort
app = Flask(__name__)


@app.route("/metrics")
def get_metrics():
    """
    run zpool_prometheus and, if successful, return the results

    :return:
    """
    res = ''
    try:
        res = check_output(['zpool_prometheus'])
    except Exception:
        abort(500)
    return res
