from distutils.core import setup, Extension
from Cython.Build import cythonize

setup(
    ext_modules = cythonize([
		Extension(
			"basic_nodes",
			sources=["basic_nodes.pyx"],
			language="c++",
		),
	],
	annotate=True
	)
)
