
// Boost substitute. For full boost library see http://boost.org

#ifndef BOOST_STATIC_ASSERT_HPP
#define BOOST_STATIC_ASSERT_HPP

#define BOOST_STATIC_ASSERT3( expr, line ) \
			typedef int boost_static_assert_##line [1 / ((expr) ? 1 : 0)]

#define BOOST_STATIC_ASSERT2( expr, line ) BOOST_STATIC_ASSERT3( expr, line )

#define BOOST_STATIC_ASSERT( expr ) BOOST_STATIC_ASSERT2( expr, __LINE__ )

#endif

