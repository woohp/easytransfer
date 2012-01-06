env = Environment(
    CXX = 'g++',
#    CXXFLAGS = ['-std=c++0x', '-stdlib=libc++', '-Wall', '-pedantic', '-g'],
    CXXFLAGS = ['-Wall', '-pedantic', '-g'],
    LIBS = ['boost_filesystem-mt', 'boost_system-mt'],
    CPPPATH = '.'
)

icmp = env.Program('httpserver', ['httpserver.cpp', 'mongoose.c'])
