env = Environment(
    CXX = 'g++',
#    CXXFLAGS = ['-std=c++0x', '-stdlib=libc++', '-Wall', '-pedantic', '-g'],
    CXXFLAGS = ['-Wall', '-pedantic', '-g'],
    LIBS = ['boost_filesystem-mt', 'boost_system-mt', 'boost_program_options-mt', 'miniupnpc', 'dl', 'archive'],
    CPPPATH = '.'
)

httpserver = env.Program('httpserver', ['httpserver.cpp', 'mongoose.c'])

# for installation
env.Alias('install', '/usr/local/bin')
env.Install('/usr/local/bin', httpserver)
share = env.InstallAs('/usr/local/bin/share', 'share.rb')
env.AddPostAction(share, Chmod(str(share[0]), 0755))

# for uninstallation
# env.Command("uninstall-all", None, Delete(FindInstalledFiles()))
# env.Alias('uninstall', 'uninstall-all')
