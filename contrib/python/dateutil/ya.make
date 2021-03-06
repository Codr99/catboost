PY23_LIBRARY(dateutil)



LICENSE(Apache-2.0)
VERSION(2.8.0)

PEERDIR(
    contrib/python/six
    library/python/resource
)

PY_SRCS(
    TOP_LEVEL
    dateutil/__init__.py
    dateutil/_common.py
    dateutil/_version.py
    dateutil/easter.py
    dateutil/parser/__init__.py
    dateutil/parser/_parser.py
    dateutil/parser/isoparser.py
    dateutil/relativedelta.py
    dateutil/rrule.py
    dateutil/tz/__init__.py
    dateutil/tz/_common.py
    dateutil/tz/_factories.py
    dateutil/tz/tz.py
    dateutil/tz/win.py
    dateutil/tzwin.py
    dateutil/utils.py
    dateutil/zoneinfo/__init__.py
    dateutil/zoneinfo/rebuild.py
)

RESOURCE(
    dateutil/zoneinfo/dateutil-zoneinfo.tar.gz  zoneinfo/dateutil-zoneinfo.tar.gz
)

NO_LINT()

END()

RECURSE_FOR_TESTS(
    tests
)
