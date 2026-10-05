#include "../../etc/creator-test/mod-creator-test/src/CreatorTestProtocol.hpp"
#include <sstream>
#include <limits>
#include <iostream>
#include <stdexcept>
void check(bool pass,char const* message){if(!pass)throw std::runtime_error(message);}
int main(){
  try {
    CreatorTest::Request request{"0123456789abcdef0123456789abcdef",7,3,0,-9465,64,56,1,1600};
    std::stringstream encoded;CreatorTest::write(encoded,request);CreatorTest::Request decoded;
    check(CreatorTest::read(encoded,decoded,1000),"Request round trip failed");
    check(decoded.x==request.x&&decoded.token==request.token,"Request fields changed");
    check(CreatorTest::matches(decoded,7,3,1000),"Correct character rejected");
    check(!CreatorTest::matches(decoded,8,3,1000),"Wrong character accepted");
    check(!CreatorTest::matches(decoded,7,4,1000),"Wrong account accepted");
    check(!CreatorTest::matches(decoded,7,3,1600),"Expired request accepted");
    decoded.x=std::numeric_limits<double>::quiet_NaN();check(!CreatorTest::valid(decoded,1000),"NaN accepted");
    decoded=request;decoded.orientation=-1;check(!CreatorTest::valid(decoded,1000),"Invalid orientation accepted");
    decoded=request;decoded.expires=4000;check(!CreatorTest::valid(decoded,1000),"Unbounded expiry accepted");
    std::stringstream trailing;CreatorTest::write(trailing,request);trailing<<"unwanted command";
    check(!CreatorTest::read(trailing,decoded,1000),"Trailing protocol data accepted");
    std::stringstream wrongVersion;CreatorTest::write(wrongVersion,request);auto text=wrongVersion.str();text[0]='2';std::stringstream unsupported(text);
    check(!CreatorTest::read(unsupported,decoded,1000),"Unknown protocol accepted");
    check(CreatorTest::localOnly(true,"127.0.0.1","127.0.0.1;db","127.0.0.1;db","127.0.0.1;db"),"Local runtime rejected");
    check(!CreatorTest::localOnly(false,"127.0.0.1","127.0.0.1;db","127.0.0.1;db","127.0.0.1;db"),"Disabled module enabled");
    check(!CreatorTest::localOnly(true,"0.0.0.0","127.0.0.1;db","127.0.0.1;db","127.0.0.1;db"),"Public server accepted");
    check(!CreatorTest::localOnly(true,"127.0.0.1","production;db","127.0.0.1;db","127.0.0.1;db"),"Remote database accepted");
    std::cout<<"Creator test protocol checks passed\n";return 0;
  }catch(std::exception const& e){std::cerr<<e.what()<<'\n';return 1;}
}
