#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2024 UPBGE Contributors
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Integration tests for UPBGE Manifold 3D wrapper and Python C API enhancements.

Tests the architectural improvements made to the ManifoldWrapper including:
- Thread-safe operations
- Error handling infrastructure  
- Python C API integration
- Validation and safety checks
"""

import unittest
import sys

class ManifoldIntegrationTest(unittest.TestCase):
    """Test suite for Manifold integration and Python C API enhancements"""

    def setUp(self):
        """Set up test fixtures"""
        # Sample mesh data for testing
        self.valid_cube_mesh = {
            'vertices': [
                -1.0, -1.0, -1.0,  # 0
                 1.0, -1.0, -1.0,  # 1
                 1.0,  1.0, -1.0,  # 2
                -1.0,  1.0, -1.0,  # 3
                -1.0, -1.0,  1.0,  # 4
                 1.0, -1.0,  1.0,  # 5
                 1.0,  1.0,  1.0,  # 6
                -1.0,  1.0,  1.0   # 7
            ],
            'indices': [
                # Bottom face
                0, 1, 2,  0, 2, 3,
                # Top face
                4, 7, 6,  4, 6, 5,
                # Front face
                0, 4, 5,  0, 5, 1,
                # Back face
                2, 6, 7,  2, 7, 3,
                # Left face
                0, 3, 7,  0, 7, 4,
                # Right face
                1, 5, 6,  1, 6, 2
            ]
        }
        
        self.invalid_mesh = {
            'vertices': [],  # Empty - should be invalid
            'indices': []
        }
        
        self.malformed_mesh = {
            'vertices': [1.0, 2.0],  # Not divisible by 3
            'indices': [0, 1, 2, 3]  # Not divisible by 3
        }

    def test_upbge_enhanced_import(self):
        """Test that upbge_enhanced module can be imported"""
        try:
            import upbge_enhanced
            self.assertTrue(hasattr(upbge_enhanced, 'mesh_operations'))
            self.assertTrue(hasattr(upbge_enhanced, 'create_object'))
            self.assertTrue(hasattr(upbge_enhanced, 'parallel_for_each'))
        except ImportError:
            self.skipTest("upbge_enhanced module not available - build with WITH_PYTHON_C_API=ON")

    def test_mesh_validation(self):
        """Test mesh validation through Python API"""
        try:
            import upbge_enhanced
            
            # Test valid mesh
            result = upbge_enhanced.mesh_operations(self.valid_cube_mesh, "validate")
            self.assertIsInstance(result, dict)
            self.assertIn('valid', result)
            self.assertTrue(result['valid'], "Valid cube mesh should pass validation")
            self.assertIsNone(result.get('error'))
            
            # Test invalid mesh
            result = upbge_enhanced.mesh_operations(self.invalid_mesh, "validate")
            self.assertIsInstance(result, dict)
            self.assertIn('valid', result)
            self.assertFalse(result['valid'], "Empty mesh should fail validation")
            self.assertIsNotNone(result.get('error'))
            
        except ImportError:
            self.skipTest("upbge_enhanced module not available")

    def test_mesh_simplification(self):
        """Test mesh simplification through Python API"""
        try:
            import upbge_enhanced
            
            result = upbge_enhanced.mesh_operations(self.valid_cube_mesh, "simplify")
            self.assertIsInstance(result, dict)
            
            if 'error' in result and result['error'] is not None:
                # If there's an error, it should be a valid error message
                self.assertIsInstance(result['error'], str)
                self.assertGreater(len(result['error']), 0)
            else:
                # If successful, should have vertices and indices
                self.assertIn('vertices', result)
                self.assertIn('indices', result)
                self.assertIsInstance(result['vertices'], list)
                self.assertIsInstance(result['indices'], list)
                
                # Simplified mesh should still be valid geometry
                self.assertEqual(len(result['vertices']) % 3, 0, "Vertices should be groups of 3")
                self.assertEqual(len(result['indices']) % 3, 0, "Indices should be groups of 3")
                
        except ImportError:
            self.skipTest("upbge_enhanced module not available")

    def test_error_handling(self):
        """Test comprehensive error handling"""
        try:
            import upbge_enhanced
            
            # Test malformed mesh data
            result = upbge_enhanced.mesh_operations(self.malformed_mesh, "validate")
            self.assertIsInstance(result, dict)
            self.assertFalse(result.get('valid', True), "Malformed mesh should fail validation")
            
            # Test invalid operation
            result = upbge_enhanced.mesh_operations(self.valid_cube_mesh, "invalid_operation")
            self.assertIsInstance(result, dict)
            self.assertIn('error', result)
            self.assertIsNotNone(result['error'])
            self.assertIn('Unknown operation', result['error'])
            
        except ImportError:
            self.skipTest("upbge_enhanced module not available")

    def test_input_validation(self):
        """Test input parameter validation"""
        try:
            import upbge_enhanced
            
            # Test with non-dictionary input
            with self.assertRaises(TypeError):
                upbge_enhanced.mesh_operations("not_a_dict", "validate")
            
            # Test with missing keys
            incomplete_mesh = {'vertices': [1.0, 2.0, 3.0]}
            with self.assertRaises(ValueError):
                upbge_enhanced.mesh_operations(incomplete_mesh, "validate")
                
            # Test with wrong data types
            wrong_type_mesh = {
                'vertices': "not_a_list",
                'indices': [0, 1, 2]
            }
            with self.assertRaises(TypeError):
                upbge_enhanced.mesh_operations(wrong_type_mesh, "validate")
                
        except ImportError:
            self.skipTest("upbge_enhanced module not available")

    def test_metadata_reporting(self):
        """Test that operations return proper metadata"""
        try:
            import upbge_enhanced
            
            result = upbge_enhanced.mesh_operations(self.valid_cube_mesh, "validate")
            self.assertIsInstance(result, dict)
            
            # Should include operation metadata
            self.assertIn('operation', result)
            self.assertEqual(result['operation'], 'validate')
            self.assertIn('input_vertex_count', result)
            self.assertIn('input_index_count', result)
            
            # Verify counts
            self.assertEqual(result['input_vertex_count'], len(self.valid_cube_mesh['vertices']))
            self.assertEqual(result['input_index_count'], len(self.valid_cube_mesh['indices']))
            
        except ImportError:
            self.skipTest("upbge_enhanced module not available")

    def test_parallel_operations(self):
        """Test TBB-powered parallel operations"""
        try:
            import upbge_enhanced
            
            # Create a sequence of test data
            test_sequence = [1, 2, 3, 4, 5]
            
            def square_function(x):
                return x * x
            
            result = upbge_enhanced.parallel_for_each(test_sequence, square_function)
            self.assertIsInstance(result, list)
            self.assertEqual(len(result), len(test_sequence))
            
            expected = [1, 4, 9, 16, 25]
            self.assertEqual(result, expected)
            
        except ImportError:
            self.skipTest("upbge_enhanced module not available")

    def test_object_creation_api(self):
        """Test enhanced object creation API"""
        try:
            import upbge_enhanced
            
            # Test basic object creation call
            # Note: This may return None in test environment, but should not crash
            result = upbge_enhanced.create_object("test_object")
            # Just verify the call doesn't crash - actual object creation requires full UPBGE context
            
        except ImportError:
            self.skipTest("upbge_enhanced module not available")

    def test_thread_safety_simulation(self):
        """Simulate concurrent access to test thread safety"""
        try:
            import upbge_enhanced
            import threading
            import time
            
            results = []
            errors = []
            
            def worker():
                try:
                    for _ in range(10):
                        result = upbge_enhanced.mesh_operations(self.valid_cube_mesh, "validate")
                        results.append(result)
                        time.sleep(0.001)  # Small delay to increase concurrency chance
                except Exception as e:
                    errors.append(str(e))
            
            # Create multiple threads
            threads = []
            for _ in range(4):
                thread = threading.Thread(target=worker)
                threads.append(thread)
                thread.start()
            
            # Wait for all threads
            for thread in threads:
                thread.join()
            
            # Verify no errors occurred
            self.assertEqual(len(errors), 0, f"Thread safety test failed with errors: {errors}")
            self.assertGreater(len(results), 0, "No results from threaded operations")
            
            # All results should be successful
            for result in results:
                self.assertTrue(result.get('valid', False), "All validation results should be True")
                
        except ImportError:
            self.skipTest("upbge_enhanced module not available")


class ManifoldCompilationTest(unittest.TestCase):
    """Test that Manifold features are properly compiled and accessible"""
    
    def test_build_configuration(self):
        """Test that required build options are available"""
        # This test documents the expected build configuration
        required_options = [
            "WITH_PYTHON=ON",
            "WITH_PYTHON_C_API=ON", 
            "WITH_MANIFOLD=ON"
        ]
        
        # Print build requirements for documentation
        print("\n" + "="*60)
        print("MANIFOLD INTEGRATION BUILD REQUIREMENTS:")
        print("="*60)
        for option in required_options:
            print(f"  {option}")
        print("\nTo build with Manifold support:")
        print("cmake -DWITH_PYTHON_C_API=ON -DWITH_MANIFOLD=ON ..")
        print("="*60)

    def test_manifold_availability(self):
        """Test if Manifold functionality is available"""
        try:
            import upbge_enhanced
            
            # If module is available, test basic functionality
            test_mesh = {
                'vertices': [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0],
                'indices': [0, 1, 2]
            }
            
            result = upbge_enhanced.mesh_operations(test_mesh, "validate")
            self.assertIsInstance(result, dict)
            print(f"\n✓ Manifold integration is working: {result}")
            
        except ImportError:
            print("\n⚠ Manifold integration not available - check build configuration")
            print("   Build with: cmake -DWITH_PYTHON_C_API=ON -DWITH_MANIFOLD=ON ..")


def run_tests():
    """Run all Manifold integration tests"""
    # Set up test suite
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    
    # Add test cases
    suite.addTests(loader.loadTestsFromTestCase(ManifoldIntegrationTest))
    suite.addTests(loader.loadTestsFromTestCase(ManifoldCompilationTest))
    
    # Run tests with verbose output
    runner = unittest.TextTestRunner(verbosity=2, stream=sys.stdout)
    result = runner.run(suite)
    
    # Return success/failure
    return result.wasSuccessful()


if __name__ == '__main__':
    success = run_tests()
    sys.exit(0 if success else 1)