#include "term/Output.h"

#include <iostream>

namespace rv::term
{

void writeRaw(std::string_view bytes, bool flush)
{
	std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	if(flush)
	{
		std::cout.flush();
	}
}

} // namespace rv::term
