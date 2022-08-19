/*******************************************************************************
 * Copyright 2009-2016 Jörg Müller
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ******************************************************************************/

#include "util/StreamBuffer.h"
#include "util/BufferReader.h"
#include "util/Buffer.h"

#include <algorithm>

// 5 sec * 48000 samples/sec * 4 bytes/sample * 6 channels
#define BUFFER_RESIZE_BYTES 5760000
// 90 min * 60 sec/min * 48000 samples/sec * 4 bytes/sample * 2 channels
#define MAXIMUM_INITIAL_BUFFER_SIZE_BYTES 2073600000

AUD_NAMESPACE_BEGIN

StreamBuffer::StreamBuffer(std::shared_ptr<ISound> sound) :
	m_buffer(new Buffer())
{
	std::shared_ptr<IReader> reader = sound->createReader();

	m_specs = reader->getSpecs();

	int sample_size = AUD_SAMPLE_SIZE(m_specs);
	int length;
	long long index = 0;
	bool eos = false;

	// get an approximated size if possible
	long long size = std::min(reader->getLength(), MAXIMUM_INITIAL_BUFFER_SIZE_BYTES / sample_size);
	long long size_increase = BUFFER_RESIZE_BYTES / sample_size;

	if(size <= 0)
		size = size_increase;
	else
		size += m_specs.rate;

	// as long as the end of the stream is not reached
	while(!eos)
	{
		// increase
		m_buffer->resize(static_cast<long long>(size) * sample_size, true);

		// read more
		length = size-index;
		reader->read(length, eos, m_buffer->getBuffer() + index * m_specs.channels);
		if(index == m_buffer->getSize() / sample_size)
		{
			size += size_increase;
			size_increase <<= 1;
		}
		index += length;
	}

	m_buffer->resize(index * sample_size, true);
}

StreamBuffer::StreamBuffer(std::shared_ptr<Buffer> buffer, Specs specs) :
	m_buffer(buffer), m_specs(specs)
{
}

std::shared_ptr<Buffer> StreamBuffer::getBuffer()
{
	return m_buffer;
}

Specs StreamBuffer::getSpecs()
{
	return m_specs;
}

std::shared_ptr<IReader> StreamBuffer::createReader()
{
	return std::shared_ptr<IReader>(new BufferReader(m_buffer, m_specs));
}

AUD_NAMESPACE_END
